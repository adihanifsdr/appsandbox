/*
 * asb_transport.c — AppSandbox guest transport (PC: AF_HYPERV; Mac: ivshmem shared memory).
 * See asb_transport.h + TODO/asb-transport-design.md. No third-party code.
 *
 * Build: part of each guest binary (agent, the 4 channel EXEs, p9copy, vdd). Links setupapi/ws2_32.
 * NOTE: written from the validated ivshmem primitive + the exact agent.c socket surface; needs a
 * build+run pass on the dev VM (the ivshmem ring path especially).
 */
#include "asb_transport.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN     /* keep windows.h from pulling in the old winsock.h */
#endif
#include <winsock2.h>          /* must precede windows.h */
#include <ws2tcpip.h>
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ivshmem/asb_ivshmem.h"   /* GUID_DEVINTERFACE_ASB_IVSHMEM, IOCTL_ASB_IVSHMEM_MAP */

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "advapi32.lib")   /* registry (transport override) */

/* ASB_SLOT_HDR comes from asb_transport.h (shared with the host). */
#define ASB_RING_HDR   ((uint32_t)sizeof(AsbRing))  /* head/tail/cap/pad = 24 */

/* ================================================================================
 * Backend state.
 * ================================================================================ */
static int            g_is_ivshmem = 0;
static uint8_t       *g_bar = NULL;        /* mapped BAR base (Mac) */
static uint64_t       g_bar_size = 0;
static AsbShmDirectory *g_dir = NULL;

/* ---- AF_HYPERV definitions (PC backend), matching agent.c ---- */
#define AF_HYPERV 34
typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT         Reserved;
    GUID           VmId;
    GUID           ServiceId;
} SOCKADDR_HV;
static const GUID HV_GUID_WILDCARD = { 0,0,0,{0,0,0,0,0,0,0,0} };

/* channel -> a5b0cafe-000N-4000-8000-000000000001 (N = channel, low 16 bits). 9P uses a different
 * template on PC (p9copy's make_vsock_guid) — TODO when the PC 9P path is wired; Mac 9P uses the region. */
static GUID hv_service_guid(int channel) {
    GUID g = { 0xa5b0cafe, 0x0000, 0x4000, { 0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x01 } };
    g.Data2 = (unsigned short)channel;
    return g;
}

/* ================================================================================
 * Handles.
 * ================================================================================ */
struct AsbListener {
    int       channel;
    /* PC */
    SOCKET    sock;
    /* Mac */
    AsbShmRegionDesc *region;
};
struct AsbConn {
    int       channel;
    int       is_ivshmem;
    /* PC */
    SOCKET    sock;
    /* Mac: pointers into the BAR for this slot */
    volatile uint32_t *state;
    AsbRing  *g2h; uint8_t *g2h_data;   /* guest writes (send) */
    AsbRing  *h2g; uint8_t *h2g_data;   /* guest reads  (recv) */
};

/* ================================================================================
 * ivshmem helpers.
 * ================================================================================ */
static AsbShmRegionDesc *find_region(int channel) {
    uint32_t i;
    if (!g_dir) return NULL;
    for (i = 0; i < g_dir->n_regions && i < 16; i++)
        if (g_dir->regions[i].channel_id == (uint32_t)channel) return &g_dir->regions[i];
    return NULL;
}

/* Resolve slot index -> the AsbConn ring pointers (host has laid out g2h then h2g, each hdr+cap). */
static void bind_slot(AsbConn *c, AsbShmRegionDesc *r, uint32_t slot_index) {
    uint8_t *slot = g_bar + r->offset + (uint64_t)slot_index * r->slot_stride;
    c->state    = (volatile uint32_t *)slot;
    c->g2h      = (AsbRing *)(slot + ASB_SLOT_HDR);
    c->g2h_data = (uint8_t *)c->g2h + ASB_RING_HDR;
    c->h2g      = (AsbRing *)(c->g2h_data + c->g2h->cap);
    c->h2g_data = (uint8_t *)c->h2g + ASB_RING_HDR;
}

/* SPSC ring: write up to len bytes; returns bytes written (0 if full). Single producer. */
static int ring_write(AsbRing *r, uint8_t *data, const void *buf, int len) {
    uint64_t head, tail; uint32_t cap, used, freeb, n, first;
    MemoryBarrier();
    head = r->head; tail = r->tail; cap = r->cap;
    used = (uint32_t)(tail - head); freeb = cap - used;
    n = (uint32_t)len < freeb ? (uint32_t)len : freeb;
    if (n == 0) return 0;
    first = cap - (uint32_t)(tail & (cap - 1));            /* bytes until wrap */
    if (first >= n) {
        memcpy(data + (tail & (cap - 1)), buf, n);
    } else {
        memcpy(data + (tail & (cap - 1)), buf, first);
        memcpy(data, (const uint8_t *)buf + first, n - first);
    }
    MemoryBarrier();
    r->tail = tail + n;
    return (int)n;
}

/* SPSC ring: read up to len bytes; returns bytes read (0 if empty). Single consumer. */
static int ring_read(AsbRing *r, uint8_t *data, void *buf, int len) {
    uint64_t head, tail; uint32_t cap, used, n, first;
    MemoryBarrier();
    head = r->head; tail = r->tail; cap = r->cap;
    used = (uint32_t)(tail - head);
    n = (uint32_t)len < used ? (uint32_t)len : used;
    if (n == 0) return 0;
    /* ACQUIRE. ring_write publishes `tail` with a release barrier AFTER its data memcpy, so the data is
     * globally visible before the new tail. We must mirror that: order our `tail` load (above) BEFORE
     * reading the data below. Without this, on weak memory (ARM64 guest + host) the data read can be
     * speculated ahead of the tail load and return bytes the producer hasn't written yet — silent
     * corruption that only opens while the producer is actively writing (large transfers; SSH then
     * fails with a MAC/padding error). The barrier at the top of this function is NOT sufficient: it
     * precedes the tail load instead of separating the tail load from the data read. */
    MemoryBarrier();
    first = cap - (uint32_t)(head & (cap - 1));
    if (first >= n) {
        memcpy(buf, data + (head & (cap - 1)), n);
    } else {
        memcpy(buf, data + (head & (cap - 1)), first);
        memcpy((uint8_t *)buf + first, data, n - first);
    }
    MemoryBarrier();
    r->head = head + n;
    return (int)n;
}

/* ================================================================================
 * init / backend selection.
 * ================================================================================ */
static int open_ivshmem(void) {
    HDEVINFO di; SP_DEVICE_INTERFACE_DATA ifd; SP_DEVICE_INTERFACE_DETAIL_DATA *det;
    DWORD need = 0; HANDLE h; ASB_IVSHMEM_MAP m; DWORD br = 0;
    di = SetupDiGetClassDevs(&GUID_DEVINTERFACE_ASB_IVSHMEM, NULL, NULL,
                             DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return -1;
    ifd.cbSize = sizeof(ifd);
    if (!SetupDiEnumDeviceInterfaces(di, NULL, &GUID_DEVINTERFACE_ASB_IVSHMEM, 0, &ifd)) return -1;
    SetupDiGetDeviceInterfaceDetail(di, &ifd, NULL, 0, &need, NULL);
    det = (SP_DEVICE_INTERFACE_DETAIL_DATA *)malloc(need);
    if (!det) return -1;
    det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
    if (!SetupDiGetDeviceInterfaceDetail(di, &ifd, det, need, NULL, NULL)) { free(det); return -1; }
    h = CreateFile(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    free(det);
    if (h == INVALID_HANDLE_VALUE) return -1;
    ZeroMemory(&m, sizeof(m));
    if (!DeviceIoControl(h, IOCTL_ASB_IVSHMEM_MAP, NULL, 0, &m, sizeof(m), &br, NULL)) return -1;
    g_bar = (uint8_t *)(uintptr_t)m.userVa;
    g_bar_size = m.size;
    g_dir = (AsbShmDirectory *)g_bar;
    if (g_dir->magic != ASB_SHM_DIR_MAGIC) return -1;   /* host hasn't laid out the directory */
    return 0;
}

int asb_transport_init(void) {
    static int inited = 0;
    WSADATA wsa;
    char val[16]; DWORD vlen = sizeof(val); HKEY k; int force_hv = 0, force_iv = 0;
    if (inited) return 0;        /* idempotent: multiple callers in one process (agent + p9copy) */
    inited = 1;
    /* 1. explicit override */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\AppSandbox", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, "Transport", NULL, NULL, (BYTE *)val, &vlen) == ERROR_SUCCESS) {
            if (_stricmp(val, "ivshmem") == 0) force_iv = 1;
            else if (_stricmp(val, "hyperv") == 0) force_hv = 1;
        }
        RegCloseKey(k);
    }
    WSAStartup(MAKEWORD(2, 2), &wsa);   /* needed by PC backend + the ssh/9p TCP relays either way */
    if (!force_hv && (force_iv || open_ivshmem() == 0)) { g_is_ivshmem = 1; return 0; }
    g_is_ivshmem = 0;                   /* default: AF_HYPERV */
    return 0;
}

int asb_transport_is_ivshmem(void) { return g_is_ivshmem; }

/* ================================================================================
 * Stream API.
 * ================================================================================ */
AsbListener *asb_listen(int channel) {
    AsbListener *l = (AsbListener *)calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->channel = channel;
    l->sock = INVALID_SOCKET;
    if (g_is_ivshmem) {
        l->region = find_region(channel);
        if (!l->region) { free(l); return NULL; }
        return l;
    }
    {   /* PC: socket + bind(GUID) + listen. Retry bind 10x/500ms — matches the channel
           EXEs' original behaviour (the service GUID can be briefly held after a respawn). */
        SOCKADDR_HV addr; int tries; int bound = 0;
        l->sock = socket(AF_HYPERV, SOCK_STREAM, 1);
        if (l->sock == INVALID_SOCKET) { free(l); return NULL; }
        memset(&addr, 0, sizeof(addr));
        addr.Family = AF_HYPERV; addr.VmId = HV_GUID_WILDCARD; addr.ServiceId = hv_service_guid(channel);
        for (tries = 0; tries < 10; tries++) {
            if (bind(l->sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) { bound = 1; break; }
            Sleep(500);
        }
        if (!bound || listen(l->sock, 4) != 0) {
            closesocket(l->sock); free(l); return NULL;
        }
        return l;
    }
}

AsbConn *asb_accept(AsbListener *l, int timeout_ms) {
    if (!l) return NULL;
    if (g_is_ivshmem) {
        /* poll slots for one the connector set to CONNECTING; claim it */
        int elapsed = 0;
        for (;;) {
            uint32_t s;
            for (s = 0; s < l->region->n_slots; s++) {
                AsbConn tmp; bind_slot(&tmp, l->region, s);
                MemoryBarrier();
                if (*tmp.state == ASB_SLOT_CONNECTING) {
                    AsbConn *c = (AsbConn *)calloc(1, sizeof(*c));
                    if (!c) return NULL;
                    *c = tmp; c->channel = l->channel; c->is_ivshmem = 1;
                    *c->state = ASB_SLOT_ESTABLISHED; MemoryBarrier();
                    return c;
                }
            }
            if (timeout_ms >= 0 && elapsed >= timeout_ms) return NULL;
            Sleep(1); elapsed += 1;
        }
    }
    {   /* PC: select + accept */
        fd_set fds; struct timeval tv; SOCKET cs;
        FD_ZERO(&fds); FD_SET(l->sock, &fds);
        tv.tv_sec = timeout_ms / 1000; tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(0, &fds, NULL, NULL, timeout_ms < 0 ? NULL : &tv) <= 0) return NULL;
        cs = accept(l->sock, NULL, NULL);
        if (cs == INVALID_SOCKET) return NULL;
        { AsbConn *c = (AsbConn *)calloc(1, sizeof(*c)); if (!c) { closesocket(cs); return NULL; } c->channel = l->channel; c->sock = cs; return c; }
    }
}

AsbConn *asb_connect(int channel) {   /* guest connects out (9P) */
    if (g_is_ivshmem) {
        AsbShmRegionDesc *r = find_region(channel); uint32_t s;
        if (!r) return NULL;
        for (s = 0; s < r->n_slots; s++) {
            AsbConn tmp; bind_slot(&tmp, r, s);
            MemoryBarrier();
            if (*tmp.state == ASB_SLOT_FREE) {
                int waited = 0;
                *tmp.state = ASB_SLOT_CONNECTING; MemoryBarrier();
                while (*(volatile uint32_t *)tmp.state != ASB_SLOT_ESTABLISHED) {
                    if (waited > 5000) { *tmp.state = ASB_SLOT_FREE; return NULL; }
                    Sleep(1); waited++; MemoryBarrier();
                }
                { AsbConn *c = (AsbConn *)calloc(1, sizeof(*c)); if (!c) { *tmp.state = ASB_SLOT_FREE; return NULL; } *c = tmp; c->channel = channel; c->is_ivshmem = 1; return c; }
            }
        }
        return NULL;   /* no free slot */
    }
    {   /* PC: socket + connect (VmId=parent for guest->host) — 9P template TODO from p9copy.c */
        return NULL;
    }
}

unsigned long long asb_conn_socket_u64(AsbConn *c) {
    if (!c || c->is_ivshmem) return ~0ull;   /* INVALID_SOCKET — no PC socket */
    return (unsigned long long)c->sock;
}

int asb_send(AsbConn *c, const void *buf, int len) {
    if (!c) return -1;
    if (c->is_ivshmem) {
        int off = 0;
        while (off < len) {
            int n;
            MemoryBarrier();
            /* Any state other than ESTABLISHED means the connector (host) closed or RE-ARMED the slot
               (e.g. the viewer relaunched, setting it back to CONNECTING) — treat as disconnect so the
               acceptor's serve loop returns and re-accepts, mirroring a TCP peer close on PC. */
            if (*c->state != ASB_SLOT_ESTABLISHED) return off ? off : -1;
            n = ring_write(c->g2h, c->g2h_data, (const uint8_t *)buf + off, len - off);
            if (n > 0) off += n; else Sleep(0);   /* ring full: yield, retry (host draining) */
        }
        return off;
    }
    {   /* PC: loop until all bytes are sent (a single send() can return < len on a
           stream socket) — preserves the original send_all() behaviour. */
        int off = 0;
        while (off < len) {
            int n = send(c->sock, (const char *)buf + off, len - off, 0);
            if (n <= 0) return off ? off : n;
            off += n;
        }
        return off;
    }
}

int asb_recv(AsbConn *c, void *buf, int len) {
    if (!c) return -1;
    if (c->is_ivshmem) {
        for (;;) {
            int n = ring_read(c->h2g, c->h2g_data, buf, len);
            if (n > 0) return n;             /* drain any buffered data before reporting EOF */
            MemoryBarrier();
            /* Connector closed or re-armed the slot (left ESTABLISHED) -> EOF (== TCP recv 0). */
            if (*c->state != ASB_SLOT_ESTABLISHED) return 0;
            Sleep(0);   /* empty: yield then re-poll (idle backoff handled by callers if needed) */
        }
    }
    return recv(c->sock, (char *)buf, len, 0);
}

int asb_poll(AsbConn *c, int timeout_ms) {
    if (!c) return -1;
    if (c->is_ivshmem) {
        int elapsed = 0;
        for (;;) {
            MemoryBarrier();
            if (c->h2g->tail != c->h2g->head) return 1;       /* inbound bytes available */
            if (*c->state != ASB_SLOT_ESTABLISHED) return -1; /* connector closed or re-armed -> closed */
            if (timeout_ms >= 0 && elapsed >= timeout_ms) return 0;
            Sleep(1); elapsed++;
        }
    }
    {   /* PC: select on the socket */
        fd_set f; struct timeval tv;
        FD_ZERO(&f); FD_SET(c->sock, &f);
        tv.tv_sec = timeout_ms / 1000; tv.tv_usec = (timeout_ms % 1000) * 1000;
        return select(0, &f, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
    }
}

void asb_set_timeout(AsbConn *c, int recv_ms, int send_ms) {
    if (!c || c->is_ivshmem) return;   /* ivshmem is poll-driven; nothing to set */
    setsockopt(c->sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&recv_ms, sizeof(recv_ms));
    setsockopt(c->sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&send_ms, sizeof(send_ms));
}

void asb_close(AsbConn *c) {
    if (!c) return;
    if (c->is_ivshmem) {
        /* Only signal CLOSING if WE still hold the established slot. If the connector already
           re-armed it (CONNECTING/FREE after a host relaunch) or closed it, writing CLOSING would
           clobber the connector's fresh state and strand the next accept — just free the wrapper. */
        MemoryBarrier();
        if (*c->state == ASB_SLOT_ESTABLISHED) { *c->state = ASB_SLOT_CLOSING; MemoryBarrier(); }
    }
    else if (c->sock != INVALID_SOCKET) closesocket(c->sock);
    free(c);
}

void asb_close_listener(AsbListener *l) {
    if (!l) return;
    if (!g_is_ivshmem && l->sock != INVALID_SOCKET) closesocket(l->sock);
    free(l);
}

void asb_abandon(AsbConn *c) {
    /* Free the wrapper only; leave the slot/socket untouched (the new owner controls it now). */
    if (c) free(c);
}

void asb_stream_reset(AsbConn *c) {
    if (!c || !c->is_ivshmem) return;
    c->g2h->head = c->g2h->tail = 0;
    MemoryBarrier();
    *c->state = ASB_SLOT_ESTABLISHED;
    MemoryBarrier();
}

/* ---- newline-framed helpers (== recv_line/send_line in agent.c) ---- */
int asb_recv_line(AsbConn *c, char *buf, int buf_size) {
    int pos = 0;
    while (pos < buf_size - 1) {
        char ch; int n = asb_recv(c, &ch, 1);
        if (n <= 0) return n;
        if (ch == '\n') break;
        if (ch != '\r') buf[pos++] = ch;
    }
    buf[pos] = '\0';
    return pos;
}
int asb_send_line(AsbConn *c, const char *msg) {
    int len = (int)strlen(msg), n = asb_send(c, msg, len);
    if (n > 0) n = asb_send(c, "\n", 1);
    return n;
}

/* ---- VDD frame channel (ch2): guest writes BGRA into the frame region; host polls produced_seq ----
 * Layout: AsbFrameRegion header (1 page) + n_buffers frame buffers. Double-buffer so the guest writes
 * buffer N+1 while the host reads N. The guest declares the format (width/height) in the header. */
#define ASB_FRAME_MAGIC 0x52465341u   /* 'ASFR' — matches VDD_FRAME_MAGIC */
struct AsbFrame {
    AsbFrameRegion *fr;
    uint8_t        *base;
    uint32_t        n_buffers;
    uint64_t        buf_stride;
    int             back;             /* buffer index currently handed out for writing */
};

AsbFrame *asb_frame_open(int width, int height, int n_buffers) {
    AsbShmRegionDesc *r;
    AsbFrame *f;
    uint64_t hdr_sz, buf_stride;
    if (!g_is_ivshmem) return NULL;                 /* PC keeps the ch2 hvsocket (VDD's existing path) */
    r = find_region(ASB_CH_DISPLAY);
    if (!r || !(r->flags & ASB_REGION_FRAME)) return NULL;
    if (n_buffers < 2) n_buffers = 2;
    buf_stride = (uint64_t)width * (uint64_t)height * 4ull;
    buf_stride = (buf_stride + 4095ull) & ~4095ull; /* page-align each buffer */
    hdr_sz = 4096ull;
    if (hdr_sz + (uint64_t)n_buffers * buf_stride > r->size) {
        n_buffers = (int)((r->size - hdr_sz) / buf_stride);
        if (n_buffers < 1) return NULL;
    }
    f = (AsbFrame *)calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->base = g_bar + r->offset;
    f->fr   = (AsbFrameRegion *)f->base;
    f->n_buffers = (uint32_t)n_buffers;
    f->buf_stride = buf_stride;
    f->back = 0;
    f->fr->n_buffers = (uint32_t)n_buffers;
    f->fr->width = (uint32_t)width; f->fr->height = (uint32_t)height;
    f->fr->stride = (uint32_t)width * 4u; f->fr->format = 0; /* BGRA8 */
    f->fr->buffers_offset = hdr_sz; f->fr->buffer_stride = buf_stride;
    f->fr->consumed_seq = 0; f->fr->active_buffer = 0; f->fr->dirty_rect_count = 0;
    /* Cursor area right after the buffers, if it fits. */
    {
        uint64_t cur_off = hdr_sz + (uint64_t)n_buffers * buf_stride;
        if (cur_off + ASB_CURSOR_AREA <= r->size) {
            AsbCursor *cur = (AsbCursor *)(f->base + cur_off);
            memset(cur, 0, sizeof(*cur));
            cur->magic = ASB_CURSOR_MAGIC;
            f->fr->cursor_offset = cur_off;
        } else {
            f->fr->cursor_offset = 0;
        }
    }
    MemoryBarrier();
    f->fr->produced_seq = 0;
    f->fr->magic = ASB_FRAME_MAGIC;                 /* publish header last */
    MemoryBarrier();
    return f;
}

void *asb_frame_back_buffer(AsbFrame *f) {
    if (!f) return NULL;
    f->back = (int)((f->fr->active_buffer + 1) % f->n_buffers);   /* the buffer the host isn't reading */
    return f->base + f->fr->buffers_offset + (uint64_t)f->back * f->buf_stride;
}

void asb_frame_publish(AsbFrame *f) {
    if (!f) return;
    MemoryBarrier();
    f->fr->active_buffer = (uint32_t)f->back;
    MemoryBarrier();
    f->fr->produced_seq++;                          /* host polls this */
    MemoryBarrier();
}

void asb_frame_close(AsbFrame *f) { free(f); }

void *asb_frame_cursor(AsbFrame *f) {
    if (!f || !f->fr->cursor_offset) return NULL;
    return f->base + f->fr->cursor_offset;
}

/* ================================================================================
 * Raw region access for simple one-directional ring channels (e.g. input).
 * ================================================================================ */
void *asb_transport_region_base(int channel_id) {
    AsbShmRegionDesc *r;
    if (!g_is_ivshmem || !g_bar) return NULL;
    r = find_region(channel_id);
    if (!r) return NULL;
    return g_bar + r->offset;
}

int asb_ring_drain(void *region_base, void *buf, int len) {
    AsbRing *r = (AsbRing *)region_base;
    if (!region_base) return 0;
    return ring_read(r, (uint8_t *)region_base + sizeof(AsbRing), buf, len);
}
