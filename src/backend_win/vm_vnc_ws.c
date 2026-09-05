/*
 * vm_vnc_ws.c - WebSocket bridge for the in-app VNC viewer (noVNC).
 *
 * The page runs from file:// in WebView2 and cannot open a raw TCP socket,
 * so noVNC talks WebSocket to 127.0.0.1:<port> here; each connection is
 * upgraded (RFC 6455, "binary" subprotocol) and relayed byte for byte to
 * 127.0.0.1:<target_port>, the VNC tunnel of vm_ssh_proxy.c. Two threads
 * per connection (ws->tcp decodes frames and unmasks; tcp->ws wraps in
 * binary frames); closing either socket ends both. Ping is answered with
 * pong, close with close. Nothing is exposed beyond loopback.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include "vm_vnc_ws.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#define WS_MAX_BRIDGES   8
#define WS_HDR_MAX       8192
#define WS_RELAY_BUF     16384
#define WS_MAX_FRAME     (16u * 1024u * 1024u)

typedef struct WsBridge {
    DWORD  target_port;
    DWORD  port;
    SOCKET listen_sock;
    HANDLE thread;
} WsBridge;

typedef struct WsConn {
    SOCKET ws;
    SOCKET tcp;
    HANDLE ws2tcp;
} WsConn;

static WsBridge g_bridges[WS_MAX_BRIDGES];
static CRITICAL_SECTION g_cs;
static BOOL g_cs_init;

/* ---- SHA-1 (RFC 3174) + base64, for the handshake only ---- */

static void sha1(const unsigned char *msg, size_t len, unsigned char out[20])
{
    unsigned int h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    size_t total = ((len + 8) / 64 + 1) * 64;
    unsigned char *buf = (unsigned char *)calloc(total, 1);
    size_t off;
    unsigned long long bits = (unsigned long long)len * 8;
    int i;

    if (!buf) { memset(out, 0, 20); return; }
    memcpy(buf, msg, len);
    buf[len] = 0x80;
    for (i = 0; i < 8; i++) buf[total - 1 - i] = (unsigned char)(bits >> (8 * i));

    for (off = 0; off < total; off += 64) {
        unsigned int w[80], a, b, c, d, e, f, k, t;
        for (i = 0; i < 16; i++)
            w[i] = ((unsigned int)buf[off + i * 4] << 24) | ((unsigned int)buf[off + i * 4 + 1] << 16) |
                   ((unsigned int)buf[off + i * 4 + 2] << 8) | (unsigned int)buf[off + i * 4 + 3];
        for (i = 16; i < 80; i++) {
            unsigned int x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (x << 1) | (x >> 31);
        }
        a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
        for (i = 0; i < 80; i++) {
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
            t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    free(buf);
    for (i = 0; i < 5; i++) {
        out[i * 4]     = (unsigned char)(h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(h[i]);
    }
}

static void base64(const unsigned char *in, size_t len, char *out)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;
    for (i = 0; i < len; i += 3) {
        unsigned int v = (unsigned int)in[i] << 16;
        if (i + 1 < len) v |= (unsigned int)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
}

/* ---- socket helpers ---- */

static BOOL send_all(SOCKET s, const void *data, size_t len)
{
    const char *p = (const char *)data;
    while (len > 0) {
        int n = send(s, p, (int)(len > 65536 ? 65536 : len), 0);
        if (n <= 0) return FALSE;
        p += n; len -= (size_t)n;
    }
    return TRUE;
}

static BOOL recv_exact(SOCKET s, void *data, size_t len)
{
    char *p = (char *)data;
    while (len > 0) {
        int n = recv(s, p, (int)(len > 65536 ? 65536 : len), 0);
        if (n <= 0) return FALSE;
        p += n; len -= (size_t)n;
    }
    return TRUE;
}

/* A WebSocket frame from server to client: FIN + opcode, unmasked. */
static BOOL ws_send_frame(SOCKET s, unsigned char opcode, const void *payload, size_t len)
{
    unsigned char hdr[10];
    size_t hl = 2;
    hdr[0] = (unsigned char)(0x80 | opcode);
    if (len < 126) {
        hdr[1] = (unsigned char)len;
    } else if (len <= 0xFFFF) {
        hdr[1] = 126; hdr[2] = (unsigned char)(len >> 8); hdr[3] = (unsigned char)len; hl = 4;
    } else {
        int i;
        hdr[1] = 127;
        for (i = 0; i < 8; i++) hdr[2 + i] = (unsigned char)((unsigned long long)len >> (8 * (7 - i)));
        hl = 10;
    }
    if (!send_all(s, hdr, hl)) return FALSE;
    return len == 0 || send_all(s, payload, len);
}

/* Read one frame; returns the payload (caller frees) or NULL on close/error. */
static unsigned char *ws_recv_frame(SOCKET s, unsigned char *opcode, size_t *len)
{
    unsigned char h[2], mask[4], ext[8];
    unsigned long long plen;
    unsigned char *payload;
    size_t i;

    if (!recv_exact(s, h, 2)) return NULL;
    *opcode = h[0] & 0x0F;
    plen = h[1] & 0x7F;
    if (plen == 126) {
        if (!recv_exact(s, ext, 2)) return NULL;
        plen = ((unsigned long long)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        if (!recv_exact(s, ext, 8)) return NULL;
        plen = 0;
        for (i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
    }
    if (plen > WS_MAX_FRAME) return NULL;
    if (h[1] & 0x80) {
        if (!recv_exact(s, mask, 4)) return NULL;
    } else {
        memset(mask, 0, 4);
    }
    payload = (unsigned char *)malloc((size_t)plen + 1);
    if (!payload) return NULL;
    if (plen && !recv_exact(s, payload, (size_t)plen)) { free(payload); return NULL; }
    if (h[1] & 0x80)
        for (i = 0; i < (size_t)plen; i++) payload[i] ^= mask[i & 3];
    *len = (size_t)plen;
    return payload;
}

/* ---- per-connection relay ---- */

static DWORD WINAPI ws2tcp_thread(LPVOID param)
{
    WsConn *c = (WsConn *)param;
    for (;;) {
        unsigned char op; size_t len;
        unsigned char *p = ws_recv_frame(c->ws, &op, &len);
        if (!p) break;
        if (op == 0x1 || op == 0x2 || op == 0x0) {          /* text/binary/continuation: RFB bytes */
            if (len && !send_all(c->tcp, p, len)) { free(p); break; }
        } else if (op == 0x9) {                               /* ping -> pong */
            ws_send_frame(c->ws, 0xA, p, len);
        } else if (op == 0x8) {                               /* close */
            ws_send_frame(c->ws, 0x8, p, len > 2 ? 2 : len);
            free(p);
            break;
        }
        free(p);
    }
    shutdown(c->tcp, SD_BOTH);
    closesocket(c->tcp);
    return 0;
}

static DWORD WINAPI conn_thread(LPVOID param)
{
    WsConn *c = (WsConn *)param;
    DWORD target = (DWORD)(ULONG_PTR)c->ws2tcp;   /* target port smuggled in by the accept loop */
    char hdr[WS_HDR_MAX + 1];
    int got = 0;
    char *key, *end, *proto;
    char accept_src[128], accept_b64[40], resp[512];
    unsigned char digest[20];
    struct sockaddr_in to;
    unsigned char buf[WS_RELAY_BUF];

    /* HTTP upgrade request */
    while (got < WS_HDR_MAX) {
        int n = recv(c->ws, hdr + got, WS_HDR_MAX - got, 0);
        if (n <= 0) goto out_ws;
        got += n; hdr[got] = '\0';
        if (strstr(hdr, "\r\n\r\n")) break;
    }
    key = strstr(hdr, "Sec-WebSocket-Key:");
    if (!key) key = strstr(hdr, "sec-websocket-key:");
    if (!key) goto out_ws;
    key += 18;
    while (*key == ' ') key++;
    end = key;
    while (*end && *end != '\r' && *end != '\n' && *end != ' ') end++;
    if (end - key > 60) goto out_ws;
    _snprintf_s(accept_src, sizeof(accept_src), _TRUNCATE, "%.*s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", (int)(end - key), key);
    sha1((const unsigned char *)accept_src, strlen(accept_src), digest);
    base64(digest, 20, accept_b64);
    proto = strstr(hdr, "Sec-WebSocket-Protocol:");
    _snprintf_s(resp, sizeof(resp), _TRUNCATE,
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n%s\r\n",
        accept_b64, (proto && strstr(proto, "binary")) ? "Sec-WebSocket-Protocol: binary\r\n" : "");

    /* the VNC tunnel */
    c->tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->tcp == INVALID_SOCKET) goto out_ws;
    ZeroMemory(&to, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((u_short)target);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(c->tcp, (struct sockaddr *)&to, sizeof(to)) != 0) {
        ui_log(L"VNC viewer: cannot reach the VNC tunnel on 127.0.0.1:%lu.", target);
        goto out_both;
    }
    if (!send_all(c->ws, resp, strlen(resp))) goto out_both;

    c->ws2tcp = CreateThread(NULL, 0, ws2tcp_thread, c, 0, NULL);
    if (!c->ws2tcp) goto out_both;

    /* tcp -> ws in this thread */
    for (;;) {
        int n = recv(c->tcp, (char *)buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (!ws_send_frame(c->ws, 0x2, buf, (size_t)n)) break;
    }
    ws_send_frame(c->ws, 0x8, "\x03\xe8", 2);   /* 1000: normal closure */
    shutdown(c->ws, SD_BOTH);
    closesocket(c->ws);
    c->ws = INVALID_SOCKET;
    WaitForSingleObject(c->ws2tcp, 3000);
    CloseHandle(c->ws2tcp);
    free(c);
    return 0;

out_both:
    closesocket(c->tcp);
out_ws:
    if (c->ws != INVALID_SOCKET) closesocket(c->ws);
    free(c);
    return 0;
}

/* ---- listener ---- */

static DWORD WINAPI listener_thread(LPVOID param)
{
    WsBridge *b = (WsBridge *)param;
    for (;;) {
        struct sockaddr_in from;
        int fl = sizeof(from);
        SOCKET s = accept(b->listen_sock, (struct sockaddr *)&from, &fl);
        WsConn *c;
        HANDLE t;
        if (s == INVALID_SOCKET) break;
        if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) { closesocket(s); continue; }
        c = (WsConn *)calloc(1, sizeof(*c));
        if (!c) { closesocket(s); continue; }
        c->ws = s;
        c->tcp = INVALID_SOCKET;
        c->ws2tcp = (HANDLE)(ULONG_PTR)b->target_port;
        t = CreateThread(NULL, 0, conn_thread, c, 0, NULL);
        if (t) CloseHandle(t); else { closesocket(s); free(c); }
    }
    return 0;
}

ASB_API DWORD vm_vnc_ws_port(DWORD target_port)
{
    WSADATA wsa;
    struct sockaddr_in addr;
    int alen = sizeof(addr);
    int i, slot = -1;
    WsBridge *b;

    if (!target_port) return 0;
    if (!g_cs_init) { InitializeCriticalSection(&g_cs); g_cs_init = TRUE; }
    EnterCriticalSection(&g_cs);
    for (i = 0; i < WS_MAX_BRIDGES; i++) {
        if (g_bridges[i].port && g_bridges[i].target_port == target_port) {
            DWORD p = g_bridges[i].port;
            LeaveCriticalSection(&g_cs);
            return p;
        }
        if (!g_bridges[i].port && slot < 0) slot = i;
    }
    if (slot < 0) { LeaveCriticalSection(&g_cs); return 0; }
    b = &g_bridges[slot];

    WSAStartup(MAKEWORD(2, 2), &wsa);
    b->listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (b->listen_sock == INVALID_SOCKET) { LeaveCriticalSection(&g_cs); return 0; }
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(b->listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(b->listen_sock, 4) != 0 ||
        getsockname(b->listen_sock, (struct sockaddr *)&addr, &alen) != 0) {
        closesocket(b->listen_sock);
        b->listen_sock = INVALID_SOCKET;
        LeaveCriticalSection(&g_cs);
        return 0;
    }
    b->target_port = target_port;
    b->port = ntohs(addr.sin_port);
    b->thread = CreateThread(NULL, 0, listener_thread, b, 0, NULL);
    if (!b->thread) {
        closesocket(b->listen_sock);
        b->port = 0;
        LeaveCriticalSection(&g_cs);
        return 0;
    }
    ui_log(L"VNC viewer: WebSocket bridge 127.0.0.1:%lu -> 127.0.0.1:%lu.", b->port, target_port);
    LeaveCriticalSection(&g_cs);
    return b->port;
}
