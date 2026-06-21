/* asb_viewer.m — macOS live viewer + input for the AppSandbox VDD over ivshmem.
 *
 * Maps the QEMU ivshmem backing file, publishes the asb_transport directory with:
 *   - a ch2 DISPLAY frame region (guest VDD writes BGRA frames; we render them), and
 *   - a ch3 INPUT ring (we write InputPacket records; the guest asb_input_guest drains
 *     them and replays via SendInput).
 * The result is a live, interactive Windows desktop in a Mac window over shared memory.
 *
 * Build: clang -O2 -fobjc-arc -o asb_viewer asb_viewer.m -framework Cocoa
 * Run:   asb_viewer [/tmp/ivshmem.bin]
 */
#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AudioToolbox.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include "../asb_transport.h"
#include "../asb_shm_layout.h"   /* authoritative region table + asb_shm_publish (launcher owns it) */

/* ch2 DISPLAY is now a STREAM slot (host=connector, guest VDD=acceptor). The VDD pushes the same
   wire protocol it sends over HvSocket on a Windows host: VDD_WIRE_FRAME_HEADER (+ dirty rects +
   pixels) and VDD_WIRE_CURSOR_HEADER (+ shape). We reconstruct into a persistent framebuffer. */
#define FRAME_REGION_OFF   0x10000ull
#define FRAME_REGION_SIZE  (48ull * 1024 * 1024)   /* room for the slot below; downstream offsets unchanged */
#define DISPLAY_RING_CAP   0x1000000u         /* 16 MiB g2h ring — holds a full 1080p frame + headers */
#define INPUT_REGION_OFF   (FRAME_REGION_OFF + FRAME_REGION_SIZE)
#define INPUT_REGION_SIZE  0x10000ull
#define INPUT_RING_CAP     4096u              /* per-ring data capacity, power of 2 */
#define AUDIO_REGION_OFF   (INPUT_REGION_OFF + INPUT_REGION_SIZE)
#define AUDIO_REGION_SIZE  0x100000ull        /* 1 MiB */
#define AUDIO_RING_CAP     0x40000u           /* 256 KiB per ring (buffers the PCM stream) */
#define ASB_RING_HDR       ((uint32_t)sizeof(AsbRing))

/* VDD wire protocol (mirror of tools/vdd/vdd.h + src/backend_win/vm_display_idd.c). */
#define VDD_FRAME_MAGIC    0x52465341u        /* 'ASFR' */
#define VDD_CURSOR_MAGIC   0x52435341u        /* 'ASCR' */
#define VDD_MAX_DIRTY      64u
#pragma pack(push, 1)
typedef struct { uint32_t magic, width, height, stride; uint64_t frame_seq; uint32_t dirty_rect_count; } WireFrameHeader;
typedef struct { int32_t left, top, right, bottom; } WireRect;   /* Windows RECT */
typedef struct {
    uint32_t magic; int32_t x, y; uint32_t visible, shape_updated, shape_id,
             width, height, pitch, xhot, yhot, cursor_type, shape_data_size;
} WireCursorHeader;
#pragma pack(pop)

#define AUDIO_HEADER_MAGIC 0x31415341u        /* 'ASA1' */
#pragma pack(push, 1)
typedef struct { uint32_t magic, sample_rate; uint16_t channels, bits_per_sample, format_tag, block_align; } AudioHeader;
typedef struct { uint32_t bytes; } AudioFrameHeader;
#pragma pack(pop)

/* Clipboard: ch5 (Mac->Win writer) + ch6 (Win->Mac reader). Mirrors src/backend_win/vm_clipboard.c. */
#define CLIPW_REGION_OFF   (AUDIO_REGION_OFF + AUDIO_REGION_SIZE)   /* ch5 CLIPBOARD */
#define CLIPW_REGION_SIZE  0x400000ull        /* 4 MiB */
#define CLIPR_REGION_OFF   (CLIPW_REGION_OFF + CLIPW_REGION_SIZE)   /* ch6 CLIPBOARD_READER */
#define CLIPR_REGION_SIZE  0x400000ull
#define CLIP_RING_CAP      0x100000u          /* 1 MiB per ring */
/* ch1 AGENT control channel — newline-framed text protocol (hello/heartbeat/log/idd/...). */
#define AGENT_REGION_OFF   (CLIPR_REGION_OFF + CLIPR_REGION_SIZE)   /* ch1 AGENT */
#define AGENT_REGION_SIZE  0x20000ull         /* 128 KiB */
#define AGENT_RING_CAP     0x10000u           /* 64 KiB per ring */
/* ch7 SSH proxy — multi-slot stream so concurrent ssh sessions each get a slot. Host is the
   connector (arms CONNECTING on each TCP accept); the guest agent's ssh_proxy asb_accepts. */
#define SSH_REGION_OFF     (AGENT_REGION_OFF + AGENT_REGION_SIZE)   /* ch7 SSH */
#define SSH_REGION_SIZE    0x200000ull        /* 2 MiB */
#define SSH_RING_CAP       0x20000u           /* 128 KiB per ring */
#define SSH_N_SLOTS        4u
#define SSH_LISTEN_PORT    2222               /* 127.0.0.1:2222 (ephemeral fallback) */
/* ch9P (=50001) — read-only 9P2000.L share. Guest p9copy is the connector (asb_connect arms a slot
   CONNECTING); the host is the acceptor and serves a host directory. */
#define P9_REGION_OFF      (SSH_REGION_OFF + SSH_REGION_SIZE)   /* ch9P */
#define P9_REGION_SIZE     0x400000ull        /* 4 MiB */
#define P9_RING_CAP        0x20000u           /* 128 KiB per ring (>= 9P msize 64 KiB) */
#define P9_N_SLOTS         2u
#define P9_SHARE_ROOT      "/tmp/p9share"     /* host directory served read-only */
#define CLIP_MAGIC         0x504C4341u        /* 'ACLP' */
#define CLIP_READY_MAGIC   0x59444C43u        /* 'CLDY' */
#define CLIP_MSG_FORMAT_LIST     1
#define CLIP_MSG_FORMAT_DATA_REQ 2
#define CLIP_MSG_FORMAT_DATA_RESP 3
#define CLIP_MSG_FILE_DATA       4
#define CLIP_MSG_SYNC_ENABLE     12
#define CF_UNICODETEXT_ID  13u                /* Windows CF_UNICODETEXT */
#pragma pack(push, 1)
typedef struct { uint32_t magic, msg_type, format, data_size; } ClipHeader;
#pragma pack(pop)

#define INPUT_MAGIC        0x4E495341u        /* 'ASIN' */
#define INPUT_MOUSE_MOVE   0
#define INPUT_MOUSE_BUTTON 1
#define INPUT_MOUSE_WHEEL  2
#define INPUT_KEY          3
#define INPUT_BTN_LEFT     0
#define INPUT_BTN_RIGHT    1
#define INPUT_BTN_MIDDLE   2

#pragma pack(push, 1)
typedef struct { uint32_t magic, type, param1, param2, param3; } InputPacket;
#pragma pack(pop)

static uint8_t          *g_bar      = NULL;
/* ch2 DISPLAY framebuffer, reconstructed from the VDD wire stream by the ch2 recv thread.
   The recv thread writes g_fb (+ bumps g_fbSeq under g_fbLock); the render timer copies it into
   g_renderFb when g_fbSeq advances and triggers a redraw, so drawRect never races the recv thread. */
static uint8_t          *g_fb        = NULL;   /* working buffer (recv thread) */
static uint8_t          *g_renderFb  = NULL;   /* render buffer (main thread) */
static uint32_t          g_fbW = 0, g_fbH = 0, g_fbStride = 0;
static volatile uint32_t g_fbSeq = 0;          /* bumped on each applied frame */
static uint32_t          g_renderSeq = 0;
static pthread_mutex_t   g_fbLock = PTHREAD_MUTEX_INITIALIZER;
/* ch3 input slot (host is the connector; agent's asb_accept is the acceptor). */
static volatile uint32_t *g_state   = NULL;   /* ASB_SLOT_* */
static AsbRing          *g_h2g      = NULL;   /* host writes, guest reads (recv) */
static uint8_t          *g_h2gData  = NULL;
/* ch4 audio slot (guest produces PCM into g2h; we read it). Declared here for map_and_publish. */
static volatile uint32_t *g_aState  = NULL;
static AsbRing          *g_aG2h     = NULL;
static uint8_t          *g_aG2hData = NULL;
/* ch5/ch6 clipboard slots (host is connector: send via h2g, recv via g2h). */
typedef struct { volatile uint32_t *state; AsbRing *h2g; uint8_t *h2gData; AsbRing *g2h; uint8_t *g2hData; } ClipConn;
static ClipConn g_c5;   /* ch5 CLIPBOARD (Mac->Win) */
static ClipConn g_c6;   /* ch6 CLIPBOARD_READER (Win->Mac) */
static ClipConn g_c1;   /* ch1 AGENT control (host is connector; agent's asb_accept is acceptor) */
static ClipConn g_c2;   /* ch2 DISPLAY frame stream (host is connector; VDD's asb_accept is acceptor) */
static void *agent_control_thread(void *arg);
static void *display_recv_thread(void *arg);
static void *ssh_listener_thread(void *arg);
/* ch7 SSH: region base for the N slots (host claims a FREE slot + arms CONNECTING per TCP accept). */
static uint8_t          *g_sshRegion = NULL;
static uint32_t          g_sshSlotStride = 0;
/* ch9P: region base for the N slots (guest is the connector; host accepts + serves 9P read-only). */
static void *p9_server_thread(void *arg);
static uint8_t          *g_p9Region = NULL;
static uint32_t          g_p9SlotStride = 0;

/* Guest cursor stashed by the ch2 recv thread (as an AsbCursor blob + shape bytes), applied on the
   main thread by the render timer. g_curBlob is malloc'd; guarded by g_curLock. */
static pthread_mutex_t g_curLock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t        *g_curBlob = NULL;   /* sizeof(AsbCursor) + image bytes */
static volatile uint32_t g_curBlobSeq = 0; /* bumped on shape/visibility change */
static uint32_t        g_curBlobApplied = 0;

/* ---- Host->guest SPSC ring write into the slot's h2g ring (matches ring_write) ---- */
static void ring_write_packet(const InputPacket *pkt)
{
    if (!g_state || !g_h2g) return;

    uint32_t st = *g_state;
    if (st != ASB_SLOT_ESTABLISHED) {
        /* (Re-)arm the connection if the slot is idle/closed; the agent's asb_accept
           will move CONNECTING -> ESTABLISHED once it's running. Drop input until then. */
        if (st == ASB_SLOT_FREE || st == ASB_SLOT_CLOSED || st == ASB_SLOT_CLOSING) {
            g_h2g->head = g_h2g->tail = 0;
            __sync_synchronize();
            *g_state = ASB_SLOT_CONNECTING;
        }
        return;
    }

    AsbRing *r = g_h2g;
    __sync_synchronize();
    uint64_t head = r->head, tail = r->tail;
    uint32_t cap = r->cap, used = (uint32_t)(tail - head), freeb = cap - used;
    uint32_t len = (uint32_t)sizeof(*pkt);
    if (freeb < len) return;                  /* full — drop whole packet (keep framing) */
    uint32_t pos = (uint32_t)(tail & (cap - 1));
    uint32_t first = cap - pos;
    if (first >= len) {
        memcpy(g_h2gData + pos, pkt, len);
    } else {
        memcpy(g_h2gData + pos, pkt, first);
        memcpy(g_h2gData, (const uint8_t *)pkt + first, len - first);
    }
    __sync_synchronize();
    r->tail = tail + len;
}

static void send_input(uint32_t type, uint32_t p1, uint32_t p2, uint32_t p3)
{
    InputPacket pkt = { INPUT_MAGIC, type, p1, p2, p3 };
    ring_write_packet(&pkt);
}

/* Coalesced mouse-move state (main thread only). A retina trackpad fires far more
   move events than the guest can inject, so queuing every one backs up the ring and
   grows latency unboundedly. Instead we keep only the latest position and emit at
   most one move per render tick; discrete events (clicks/keys/wheel) flush the
   pending move first so ordering is preserved. */
static int      g_hasMove = 0;
static uint32_t g_moveX = 0, g_moveY = 0;
static void flush_move(void)
{
    if (g_hasMove) { g_hasMove = 0; send_input(INPUT_MOUSE_MOVE, g_moveX, g_moveY, 0); }
}

/* ---- macOS virtual keycode -> Windows VK ---- */
static uint8_t g_vk[128];
static void build_keymap(void)
{
    memset(g_vk, 0, sizeof(g_vk));
    /* letters */
    g_vk[0x00]='A'; g_vk[0x0B]='B'; g_vk[0x08]='C'; g_vk[0x02]='D'; g_vk[0x0E]='E';
    g_vk[0x03]='F'; g_vk[0x05]='G'; g_vk[0x04]='H'; g_vk[0x22]='I'; g_vk[0x26]='J';
    g_vk[0x28]='K'; g_vk[0x25]='L'; g_vk[0x2E]='M'; g_vk[0x2D]='N'; g_vk[0x1F]='O';
    g_vk[0x23]='P'; g_vk[0x0C]='Q'; g_vk[0x0F]='R'; g_vk[0x01]='S'; g_vk[0x11]='T';
    g_vk[0x20]='U'; g_vk[0x09]='V'; g_vk[0x0D]='W'; g_vk[0x07]='X'; g_vk[0x10]='Y';
    g_vk[0x06]='Z';
    /* number row */
    g_vk[0x12]='1'; g_vk[0x13]='2'; g_vk[0x14]='3'; g_vk[0x15]='4'; g_vk[0x17]='5';
    g_vk[0x16]='6'; g_vk[0x1A]='7'; g_vk[0x1C]='8'; g_vk[0x19]='9'; g_vk[0x1D]='0';
    /* whitespace / edit */
    g_vk[0x24]=0x0D; /* Return */     g_vk[0x35]=0x1B; /* Esc */
    g_vk[0x31]=0x20; /* Space */      g_vk[0x30]=0x09; /* Tab */
    g_vk[0x33]=0x08; /* Backspace */  g_vk[0x75]=0x2E; /* Delete(fwd) */
    /* arrows */
    g_vk[0x7B]=0x25; g_vk[0x7C]=0x27; g_vk[0x7D]=0x28; g_vk[0x7E]=0x26;
    /* nav cluster */
    g_vk[0x73]=0x24; /* Home */ g_vk[0x77]=0x23; /* End */
    g_vk[0x74]=0x21; /* PgUp */ g_vk[0x79]=0x22; /* PgDn */
    /* function keys */
    g_vk[0x7A]=0x70; g_vk[0x78]=0x71; g_vk[0x63]=0x72; g_vk[0x76]=0x73;
    g_vk[0x60]=0x74; g_vk[0x61]=0x75; g_vk[0x62]=0x76; g_vk[0x64]=0x77;
    g_vk[0x65]=0x78; g_vk[0x6D]=0x79; g_vk[0x67]=0x7A; g_vk[0x6F]=0x7B;
    /* punctuation (US layout) */
    g_vk[0x29]=0xBA; g_vk[0x18]=0xBB; g_vk[0x2B]=0xBC; g_vk[0x1B]=0xBD;
    g_vk[0x2F]=0xBE; g_vk[0x2C]=0xBF; g_vk[0x32]=0xC0; g_vk[0x21]=0xDB;
    g_vk[0x2A]=0xDC; g_vk[0x1E]=0xDD; g_vk[0x27]=0xDE;
}
/* extended-key keycodes (arrows + nav + fwd-delete) */
static int is_extended(unsigned short kc)
{
    switch (kc) {
    case 0x7B: case 0x7C: case 0x7D: case 0x7E:   /* arrows */
    case 0x73: case 0x77: case 0x74: case 0x79:   /* home/end/pgup/pgdn */
    case 0x75:                                    /* fwd delete */
        return 1;
    }
    return 0;
}

/* ---- Map and publish the directory (frame + input regions) ---- */
static int map_and_publish(const char *path)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open ivshmem"); return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); return -1; }
    g_bar = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (g_bar == MAP_FAILED) { perror("mmap"); return -1; }

    AsbShmDirectory *dir = (AsbShmDirectory *)g_bar;
    /* The VM launcher publishes the directory (region table + slot caps) into the backing file
       BEFORE the guest boots, so every guest service's asb_transport_init sees the magic at start
       and selects ivshmem. As a standalone fallback (no launcher) we publish it ourselves. We never
       zero the directory page here: a transient magic=0 would race the guest services' init and
       drop them onto the AF_HYPERV fallback (the black-screen bug). */
    if (dir->magic != ASB_SHM_DIR_MAGIC) asb_shm_publish(g_bar, (uint64_t)st.st_size);
    dir->version   = ASB_SHM_VERSION;
    dir->bar_size  = (uint32_t)st.st_size;
    dir->n_regions = ASB_SHM_N_REGIONS;
    /* ch2 DISPLAY: STREAM slot. g2h carries the VDD frame/cursor wire stream (big ring); h2g unused. */
    uint32_t d2_h2g_cap = 4096u;
    uint32_t d2_stride  = ASB_SLOT_HDR + (ASB_RING_HDR + DISPLAY_RING_CAP) + (ASB_RING_HDR + d2_h2g_cap);
    dir->regions[0].channel_id  = ASB_CH_DISPLAY;
    dir->regions[0].flags       = ASB_REGION_STREAM;
    dir->regions[0].offset      = FRAME_REGION_OFF;
    dir->regions[0].size        = FRAME_REGION_SIZE;
    dir->regions[0].n_slots     = 1;
    dir->regions[0].slot_stride = d2_stride;
    uint32_t slot_stride = ASB_SLOT_HDR + 2u * (ASB_RING_HDR + INPUT_RING_CAP);
    dir->regions[1].channel_id  = ASB_CH_INPUT;
    dir->regions[1].flags       = ASB_REGION_STREAM;
    dir->regions[1].offset      = INPUT_REGION_OFF;
    dir->regions[1].size        = INPUT_REGION_SIZE;
    dir->regions[1].n_slots     = 1;
    dir->regions[1].slot_stride = slot_stride;

    /* ch2 DISPLAY slot layout — host is the connector: init rings, mark FREE, arm CONNECTING. The
       VDD's asb_accept claims it (FREE/CLOSED->CONNECTING->ESTABLISHED) and, exactly like a fresh
       HvSocket accept on a Windows host, sends a full frame first. */
    {
        uint8_t *d2 = g_bar + FRAME_REGION_OFF;
        volatile uint32_t *d2state = (volatile uint32_t *)d2;
        AsbRing *d2g2h = (AsbRing *)(d2 + ASB_SLOT_HDR);
        uint8_t *d2g2h_data = (uint8_t *)d2g2h + ASB_RING_HDR;
        AsbRing *d2h2g = (AsbRing *)(d2g2h_data + DISPLAY_RING_CAP);
        uint8_t *d2h2g_data = (uint8_t *)d2h2g + ASB_RING_HDR;
        memset(d2, 0, d2_stride);
        d2g2h->cap = DISPLAY_RING_CAP;
        d2h2g->cap = d2_h2g_cap;
        *d2state = ASB_SLOT_FREE;
        g_c2.state = d2state;
        g_c2.g2h = d2g2h; g_c2.g2hData = d2g2h_data;
        g_c2.h2g = d2h2g; g_c2.h2gData = d2h2g_data;
        __sync_synchronize();
        *d2state = ASB_SLOT_CONNECTING;
    }

    /* Lay out one connection slot: [state][g2h ring + data][h2g ring + data].
       We are the connector: init rings, mark FREE, then arm CONNECTING. */
    uint8_t *slot = g_bar + INPUT_REGION_OFF;
    volatile uint32_t *pstate = (volatile uint32_t *)slot;
    AsbRing *g2h = (AsbRing *)(slot + ASB_SLOT_HDR);
    uint8_t *g2h_data = (uint8_t *)g2h + ASB_RING_HDR;
    AsbRing *h2g = (AsbRing *)(g2h_data + INPUT_RING_CAP);
    uint8_t *h2g_data = (uint8_t *)h2g + ASB_RING_HDR;
    memset(slot, 0, slot_stride);
    g2h->cap = INPUT_RING_CAP;
    h2g->cap = INPUT_RING_CAP;
    *pstate = ASB_SLOT_FREE;
    g_state = pstate; g_h2g = h2g; g_h2gData = h2g_data;
    __sync_synchronize();
    *pstate = ASB_SLOT_CONNECTING;

    /* ch4 AUDIO stream slot (guest produces PCM into g2h; we are the connector). */
    uint32_t a_stride = ASB_SLOT_HDR + 2u * (ASB_RING_HDR + AUDIO_RING_CAP);
    dir->regions[2].channel_id  = ASB_CH_AUDIO;
    dir->regions[2].flags       = ASB_REGION_STREAM;
    dir->regions[2].offset      = AUDIO_REGION_OFF;
    dir->regions[2].size        = AUDIO_REGION_SIZE;
    dir->regions[2].n_slots     = 1;
    dir->regions[2].slot_stride = a_stride;
    {
        uint8_t *aslot = g_bar + AUDIO_REGION_OFF;
        volatile uint32_t *ast = (volatile uint32_t *)aslot;
        AsbRing *ag2h = (AsbRing *)(aslot + ASB_SLOT_HDR);
        uint8_t *ag2h_data = (uint8_t *)ag2h + ASB_RING_HDR;
        AsbRing *ah2g = (AsbRing *)(ag2h_data + AUDIO_RING_CAP);
        memset(aslot, 0, a_stride);
        ag2h->cap = AUDIO_RING_CAP;
        ah2g->cap = AUDIO_RING_CAP;
        *ast = ASB_SLOT_FREE;
        g_aState = ast; g_aG2h = ag2h; g_aG2hData = ag2h_data;
        __sync_synchronize();
        *ast = ASB_SLOT_CONNECTING;
    }

    /* ch5 (CLIPBOARD) + ch6 (CLIPBOARD_READER) stream slots — host is the connector. */
    {
        struct { int ch; uint64_t off, size; ClipConn *cc; } clips[2] = {
            { ASB_CH_CLIPBOARD,        CLIPW_REGION_OFF, CLIPW_REGION_SIZE, &g_c5 },
            { ASB_CH_CLIPBOARD_READER, CLIPR_REGION_OFF, CLIPR_REGION_SIZE, &g_c6 },
        };
        uint32_t cstride = ASB_SLOT_HDR + 2u * (ASB_RING_HDR + CLIP_RING_CAP);
        for (int i = 0; i < 2; i++) {
            int idx = 3 + i;
            dir->regions[idx].channel_id  = clips[i].ch;
            dir->regions[idx].flags       = ASB_REGION_STREAM;
            dir->regions[idx].offset      = clips[i].off;
            dir->regions[idx].size        = clips[i].size;
            dir->regions[idx].n_slots     = 1;
            dir->regions[idx].slot_stride = cstride;
            uint8_t *slot = g_bar + clips[i].off;
            volatile uint32_t *cst = (volatile uint32_t *)slot;
            AsbRing *g2h = (AsbRing *)(slot + ASB_SLOT_HDR);
            uint8_t *g2h_data = (uint8_t *)g2h + ASB_RING_HDR;
            AsbRing *h2g = (AsbRing *)(g2h_data + CLIP_RING_CAP);
            uint8_t *h2g_data = (uint8_t *)h2g + ASB_RING_HDR;
            memset(slot, 0, cstride);
            g2h->cap = CLIP_RING_CAP; h2g->cap = CLIP_RING_CAP;
            *cst = ASB_SLOT_FREE;
            clips[i].cc->state = cst;
            clips[i].cc->g2h = g2h; clips[i].cc->g2hData = g2h_data;
            clips[i].cc->h2g = h2g; clips[i].cc->h2gData = h2g_data;
            __sync_synchronize();
            *cst = ASB_SLOT_CONNECTING;
        }
    }

    /* ch1 AGENT control stream slot — host is the connector. */
    {
        uint32_t astride = ASB_SLOT_HDR + 2u * (ASB_RING_HDR + AGENT_RING_CAP);
        dir->regions[5].channel_id  = ASB_CH_AGENT;
        dir->regions[5].flags       = ASB_REGION_STREAM;
        dir->regions[5].offset      = AGENT_REGION_OFF;
        dir->regions[5].size        = AGENT_REGION_SIZE;
        dir->regions[5].n_slots     = 1;
        dir->regions[5].slot_stride = astride;
        uint8_t *slot = g_bar + AGENT_REGION_OFF;
        volatile uint32_t *cst = (volatile uint32_t *)slot;
        AsbRing *g2h = (AsbRing *)(slot + ASB_SLOT_HDR);
        uint8_t *g2h_data = (uint8_t *)g2h + ASB_RING_HDR;
        AsbRing *h2g = (AsbRing *)(g2h_data + AGENT_RING_CAP);
        uint8_t *h2g_data = (uint8_t *)h2g + ASB_RING_HDR;
        memset(slot, 0, astride);
        g2h->cap = AGENT_RING_CAP; h2g->cap = AGENT_RING_CAP;
        *cst = ASB_SLOT_FREE;
        g_c1.state = cst;
        g_c1.g2h = g2h; g_c1.g2hData = g2h_data;
        g_c1.h2g = h2g; g_c1.h2gData = h2g_data;
        __sync_synchronize();
        *cst = ASB_SLOT_CONNECTING;
    }

    /* ch7 SSH proxy — N slots, all left FREE. The ssh_listener_thread arms one CONNECTING per
       accepted TCP connection; the guest agent's ssh_proxy_thread asb_accepts it. */
    {
        g_sshSlotStride = ASB_SLOT_HDR + 2u * (ASB_RING_HDR + SSH_RING_CAP);
        g_sshRegion     = g_bar + SSH_REGION_OFF;
        dir->regions[6].channel_id  = ASB_CH_SSH;
        dir->regions[6].flags       = ASB_REGION_STREAM;
        dir->regions[6].offset      = SSH_REGION_OFF;
        dir->regions[6].size        = SSH_REGION_SIZE;
        dir->regions[6].n_slots     = SSH_N_SLOTS;
        dir->regions[6].slot_stride = g_sshSlotStride;
        for (uint32_t i = 0; i < SSH_N_SLOTS; i++) {
            uint8_t *slot = g_sshRegion + (uint64_t)i * g_sshSlotStride;
            volatile uint32_t *cst = (volatile uint32_t *)slot;
            AsbRing *g2h = (AsbRing *)(slot + ASB_SLOT_HDR);
            uint8_t *g2h_data = (uint8_t *)g2h + ASB_RING_HDR;
            AsbRing *h2g = (AsbRing *)(g2h_data + SSH_RING_CAP);
            memset(slot, 0, g_sshSlotStride);
            g2h->cap = SSH_RING_CAP; h2g->cap = SSH_RING_CAP;
            *cst = ASB_SLOT_FREE;   /* not armed — the listener arms on demand */
        }
    }

    /* ch9P read-only share — N slots, all FREE. The guest p9copy is the connector (asb_connect arms
       a slot CONNECTING); p9_server_thread accepts and serves P9_SHARE_ROOT read-only. */
    {
        g_p9SlotStride = ASB_SLOT_HDR + 2u * (ASB_RING_HDR + P9_RING_CAP);
        g_p9Region     = g_bar + P9_REGION_OFF;
        dir->regions[7].channel_id  = ASB_CH_9P;
        dir->regions[7].flags       = ASB_REGION_STREAM;
        dir->regions[7].offset      = P9_REGION_OFF;
        dir->regions[7].size        = P9_REGION_SIZE;
        dir->regions[7].n_slots     = P9_N_SLOTS;
        dir->regions[7].slot_stride = g_p9SlotStride;
        for (uint32_t i = 0; i < P9_N_SLOTS; i++) {
            uint8_t *slot = g_p9Region + (uint64_t)i * g_p9SlotStride;
            volatile uint32_t *cst = (volatile uint32_t *)slot;
            AsbRing *g2h = (AsbRing *)(slot + ASB_SLOT_HDR);
            uint8_t *g2h_data = (uint8_t *)g2h + ASB_RING_HDR;
            AsbRing *h2g = (AsbRing *)(g2h_data + P9_RING_CAP);
            memset(slot, 0, g_p9SlotStride);
            g2h->cap = P9_RING_CAP; h2g->cap = P9_RING_CAP;
            *cst = ASB_SLOT_FREE;
        }
    }

    __sync_synchronize();
    dir->magic = ASB_SHM_DIR_MAGIC;
    __sync_synchronize();
    printf("[viewer] published: ch2 frame @0x%llx, ch3 input ring @0x%llx, ch1 agent @0x%llx. Waiting for frames...\n",
           FRAME_REGION_OFF, (unsigned long long)INPUT_REGION_OFF, (unsigned long long)AGENT_REGION_OFF);
    return 0;
}

/* ================================================================================
 * Audio (ch4): the guest captures the virtual-audio loopback and streams PCM into the slot's
 * g2h ring; we drain it into an intermediate PCM ring and play it via a CoreAudio AudioQueue.
 * We are the slot connector (arm CONNECTING); the agent-spawned appsandbox-audio is the acceptor.
 * ================================================================================ */
#define PCM_RING_SZ (1u << 20)                 /* 1 MiB jitter buffer */
static uint8_t          g_pcm[PCM_RING_SZ];
static volatile uint32_t g_pcmHead = 0, g_pcmTail = 0;
static pthread_mutex_t  g_pcmLock = PTHREAD_MUTEX_INITIALIZER;
static AudioQueueRef    g_aq = NULL;

/* SPSC ring read (host consumer of g2h) — mirrors asb_transport.c ring_read. */
static int ring_read_host(AsbRing *r, uint8_t *data, void *buf, int len)
{
    __sync_synchronize();
    uint64_t head = r->head, tail = r->tail; uint32_t cap = r->cap;
    uint32_t used = (uint32_t)(tail - head);
    uint32_t n = (uint32_t)len < used ? (uint32_t)len : used;
    if (n == 0) return 0;
    uint32_t first = cap - (uint32_t)(head & (cap - 1));
    if (first >= n) memcpy(buf, data + (head & (cap - 1)), n);
    else { memcpy(buf, data + (head & (cap - 1)), first); memcpy((uint8_t *)buf + first, data, n - first); }
    __sync_synchronize();
    r->head = head + n;
    return (int)n;
}

/* Block-read exactly len bytes from the audio g2h ring; NO if the slot closed. */
static BOOL aud_read(void *buf, int len)
{
    int got = 0;
    while (got < len) {
        if (!g_aState || *g_aState == ASB_SLOT_CLOSING || *g_aState == ASB_SLOT_CLOSED) return NO;
        int n = ring_read_host(g_aG2h, g_aG2hData, (uint8_t *)buf + got, len - got);
        if (n > 0) got += n; else usleep(1000);
    }
    return YES;
}

static void pcm_push(const uint8_t *p, uint32_t n)
{
    pthread_mutex_lock(&g_pcmLock);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t next = (g_pcmTail + 1) % PCM_RING_SZ;
        if (next == g_pcmHead) g_pcmHead = (g_pcmHead + 1) % PCM_RING_SZ;  /* full → drop oldest */
        g_pcm[g_pcmTail] = p[i];
        g_pcmTail = next;
    }
    pthread_mutex_unlock(&g_pcmLock);
}
static uint32_t pcm_pull(uint8_t *out, uint32_t n)
{
    pthread_mutex_lock(&g_pcmLock);
    uint32_t got = 0;
    while (got < n && g_pcmHead != g_pcmTail) { out[got++] = g_pcm[g_pcmHead]; g_pcmHead = (g_pcmHead + 1) % PCM_RING_SZ; }
    pthread_mutex_unlock(&g_pcmLock);
    return got;
}

static void aq_cb(void *u, AudioQueueRef aq, AudioQueueBufferRef buf)
{
    (void)u;
    uint32_t cap = buf->mAudioDataBytesCapacity;
    uint32_t got = pcm_pull((uint8_t *)buf->mAudioData, cap);
    if (got < cap) memset((uint8_t *)buf->mAudioData + got, 0, cap - got);  /* pad silence on underrun */
    buf->mAudioDataByteSize = cap;
    AudioQueueEnqueueBuffer(aq, buf, 0, NULL);
}

static void audio_queue_start(const AudioHeader *h)
{
    AudioStreamBasicDescription asbd = {0};
    asbd.mSampleRate       = h->sample_rate;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = (h->format_tag == 3) ? (kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked)
                                                  : (kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked);
    asbd.mChannelsPerFrame = h->channels;
    asbd.mBitsPerChannel   = h->bits_per_sample;
    asbd.mBytesPerFrame    = h->block_align ? h->block_align : (h->channels * h->bits_per_sample / 8);
    asbd.mFramesPerPacket  = 1;
    asbd.mBytesPerPacket   = asbd.mBytesPerFrame;
    if (AudioQueueNewOutput(&asbd, aq_cb, NULL, NULL, NULL, 0, &g_aq) != noErr) { g_aq = NULL; return; }
    UInt32 bufBytes = (UInt32)(asbd.mSampleRate * 0.02) * asbd.mBytesPerFrame;  /* ~20 ms */
    if (bufBytes < 1024) bufBytes = 4096;
    for (int i = 0; i < 4; i++) {
        AudioQueueBufferRef b;
        if (AudioQueueAllocateBuffer(g_aq, bufBytes, &b) != noErr) continue;
        memset(b->mAudioData, 0, bufBytes);
        b->mAudioDataByteSize = bufBytes;
        AudioQueueEnqueueBuffer(g_aq, b, 0, NULL);
    }
    AudioQueueStart(g_aq, NULL);
}

/* Reader thread: wait for the guest, read the header, then stream PCM frames into the PCM ring. */
static void *audio_reader_thread(void *arg)
{
    (void)arg;
    for (;;) {
        if (!g_aState) { usleep(100000); continue; }
        AudioHeader h;
        if (!aud_read(&h, sizeof(h))) { usleep(50000); continue; }
        if (h.magic != AUDIO_HEADER_MAGIC) continue;
        g_pcmHead = g_pcmTail = 0;
        audio_queue_start(&h);
        for (;;) {
            AudioFrameHeader fh;
            if (!aud_read(&fh, sizeof(fh))) break;
            uint32_t remaining = fh.bytes;
            uint8_t tmp[16384];
            while (remaining > 0) {
                uint32_t chunk = remaining > sizeof(tmp) ? (uint32_t)sizeof(tmp) : remaining;
                if (!aud_read(tmp, (int)chunk)) goto closed;
                pcm_push(tmp, chunk);
                remaining -= chunk;
            }
        }
    closed:
        if (g_aq) { AudioQueueStop(g_aq, true); AudioQueueDispose(g_aq, true); g_aq = NULL; }
        /* re-arm the connector for the next guest */
        if (g_aState) {
            g_aG2h->head = g_aG2h->tail = 0;
            __sync_synchronize();
            *g_aState = ASB_SLOT_CONNECTING;
        }
        usleep(50000);
    }
    return NULL;
}

/* ================================================================================
 * Clipboard (ch5 Mac->Win writer, ch6 Win->Mac reader). Mirrors vm_clipboard.c.
 * Host is the connector on both slots: send via h2g, recv via g2h.
 * ================================================================================ */
static long g_clipSuppress = -1;   /* NSPasteboard changeCount we set (echo suppression) */
static void clog(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[clip] "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); fflush(stderr);
    va_end(ap);
}

static int ring_write_host(AsbRing *r, uint8_t *data, const void *buf, int len)
{
    __sync_synchronize();
    uint64_t head = r->head, tail = r->tail; uint32_t cap = r->cap;
    uint32_t freeb = cap - (uint32_t)(tail - head);
    uint32_t n = (uint32_t)len < freeb ? (uint32_t)len : freeb;
    if (n == 0) return 0;
    uint32_t pos = (uint32_t)(tail & (cap - 1)), first = cap - pos;
    if (first >= n) memcpy(data + pos, buf, n);
    else { memcpy(data + pos, buf, first); memcpy(data, (const uint8_t *)buf + first, n - first); }
    __sync_synchronize();
    r->tail = tail + n;
    return (int)n;
}

static BOOL clip_alive(ClipConn *c) { return c->state && *c->state != ASB_SLOT_CLOSING && *c->state != ASB_SLOT_CLOSED; }
static BOOL clip_send(ClipConn *c, const void *buf, int len)
{
    int off = 0;
    while (off < len) {
        if (!clip_alive(c)) return NO;
        int n = ring_write_host(c->h2g, c->h2gData, (const uint8_t *)buf + off, len - off);
        if (n > 0) off += n; else usleep(500);
    }
    return YES;
}
static BOOL clip_recv(ClipConn *c, void *buf, int len)
{
    int got = 0;
    while (got < len) {
        if (!clip_alive(c)) return NO;
        int n = ring_read_host(c->g2h, c->g2hData, (uint8_t *)buf + got, len - got);
        if (n > 0) got += n; else usleep(500);
    }
    return YES;
}
static uint32_t clip_avail(ClipConn *c) { __sync_synchronize(); return (uint32_t)(c->g2h->tail - c->g2h->head); }

/* Wait for the guest to accept (ESTABLISHED) and send CLIP_READY_MAGIC. */
static BOOL clip_wait_ready(ClipConn *c)
{
    while (c->state && *c->state != ASB_SLOT_ESTABLISHED) usleep(20000);
    uint32_t ready = 0;
    if (!clip_recv(c, &ready, 4)) return NO;
    return ready == CLIP_READY_MAGIC;
}
static void clip_rearm(ClipConn *c)
{
    if (!c->state) return;
    c->g2h->head = c->g2h->tail = 0;
    c->h2g->head = c->h2g->tail = 0;
    __sync_synchronize();
    *c->state = ASB_SLOT_CONNECTING;
}

/* Windows clipboard format IDs we bridge (synthetic >=0xC000 for named/registered formats). */
#define WF_TEXT   13u            /* CF_UNICODETEXT */
#define WF_DIB    8u             /* CF_DIB */
#define WF_HDROP  15u            /* CF_HDROP */
#define WF_HTML   0xC010u        /* "HTML Format" */
#define WF_RTF    0xC011u        /* "Rich Text Format" */
#pragma pack(push, 1)
typedef struct { uint32_t path_len; uint64_t file_size; uint8_t is_directory; } ClipFileInfo;
#pragma pack(pop)

/* ===== format converters ===== */
/* Windows CF_HTML <-> bare HTML fragment (NSPasteboardTypeHTML). */
static NSData *cfhtml_to_html(const uint8_t *d, uint32_t n)
{
    NSString *s = [[NSString alloc] initWithBytes:d length:n encoding:NSUTF8StringEncoding];
    if (!s) return nil;
    NSRange sf = [s rangeOfString:@"StartFragment:"], ef = [s rangeOfString:@"EndFragment:"];
    if (sf.location != NSNotFound && ef.location != NSNotFound) {
        int so = [[s substringWithRange:NSMakeRange(sf.location + 14, 10)] intValue];
        int eo = [[s substringWithRange:NSMakeRange(ef.location + 12, 10)] intValue];
        if (so >= 0 && eo > so && eo <= (int)n) return [NSData dataWithBytes:d + so length:eo - so];
    }
    return [s dataUsingEncoding:NSUTF8StringEncoding];
}
static NSData *html_to_cfhtml(NSData *frag)
{
    NSString *pre = @"<html><body><!--StartFragment-->", *post = @"<!--EndFragment--></body></html>";
    NSString *hf = @"Version:0.9\r\nStartHTML:%010d\r\nEndHTML:%010d\r\nStartFragment:%010d\r\nEndFragment:%010d\r\n";
    NSUInteger hl = [[NSString stringWithFormat:hf, 0, 0, 0, 0] lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    NSUInteger pl = [pre lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    NSUInteger fl = frag.length, ptl = [post lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    NSString *hdr = [NSString stringWithFormat:hf, (int)hl, (int)(hl + pl + fl + ptl), (int)(hl + pl), (int)(hl + pl + fl)];
    NSMutableData *out = [NSMutableData data];
    [out appendData:[hdr dataUsingEncoding:NSUTF8StringEncoding]];
    [out appendData:[pre dataUsingEncoding:NSUTF8StringEncoding]];
    [out appendData:frag];
    [out appendData:[post dataUsingEncoding:NSUTF8StringEncoding]];
    return out;
}
/* CF_DIB (BITMAPINFOHEADER + bits, no file header) <-> PNG. */
static NSData *dib_to_png(const uint8_t *d, uint32_t n)
{
    if (n < 40) return nil;
    uint32_t biSize = *(const uint32_t *)d, bpp = (n >= 16) ? *(const uint16_t *)(d + 14) : 32;
    uint32_t clrUsed = (n >= 36) ? *(const uint32_t *)(d + 32) : 0;
    uint32_t palette = (bpp <= 8) ? (clrUsed ? clrUsed : (1u << bpp)) * 4 : 0;
    uint32_t off = 14 + biSize + palette;
    NSMutableData *bmp = [NSMutableData dataWithCapacity:14 + n];
    uint8_t fh[14] = { 'B', 'M' }; uint32_t fsz = 14 + n;
    memcpy(fh + 2, &fsz, 4); memcpy(fh + 10, &off, 4);
    [bmp appendBytes:fh length:14]; [bmp appendBytes:d length:n];
    NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:bmp];
    return rep ? [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] : nil;
}
static NSData *image_to_dib(NSData *img)
{
    NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:img];
    if (!rep) return nil;
    NSData *bmp = [rep representationUsingType:NSBitmapImageFileTypeBMP properties:@{}];
    return (bmp.length > 14) ? [bmp subdataWithRange:NSMakeRange(14, bmp.length - 14)] : nil;
}

/* ===== ch6: Windows -> Mac (paste-from-Windows) ===== */
static void clip_recv_files(ClipConn *c, NSMutableArray *urls)
{
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *root = [NSTemporaryDirectory() stringByAppendingPathComponent:
                      [NSString stringWithFormat:@"asbclip-%u", arc4random()]];
    [fm createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil];
    NSMutableArray *tops = [NSMutableArray array];
    for (;;) {
        ClipHeader h;
        if (!clip_recv(c, &h, sizeof(h))) return;
        if (h.magic != CLIP_MAGIC) return;
        if (h.msg_type == CLIP_MSG_FORMAT_DATA_RESP) break;       /* terminator */
        if (h.msg_type != CLIP_MSG_FILE_DATA) {
            if (h.data_size) { uint8_t *s = malloc(h.data_size); clip_recv(c, s, h.data_size); free(s); }
            continue;
        }
        ClipFileInfo fi;
        if (!clip_recv(c, &fi, sizeof(fi))) return;
        uint8_t *pb = malloc(fi.path_len ? fi.path_len : 1);
        if (fi.path_len && !clip_recv(c, pb, fi.path_len)) { free(pb); return; }
        NSString *rel = [[NSString alloc] initWithBytes:pb length:fi.path_len encoding:NSUTF16LittleEndianStringEncoding];
        free(pb);
        rel = [rel stringByReplacingOccurrencesOfString:@"\\" withString:@"/"];
        NSString *full = [root stringByAppendingPathComponent:rel];
        NSString *top = [[rel pathComponents] firstObject];
        if (top && ![tops containsObject:top]) [tops addObject:top];
        if (fi.is_directory) {
            [fm createDirectoryAtPath:full withIntermediateDirectories:YES attributes:nil error:nil];
        } else {
            [fm createDirectoryAtPath:[full stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:nil];
            FILE *f = fopen(full.fileSystemRepresentation, "wb");
            uint64_t rem = fi.file_size; uint8_t buf[65536];
            while (rem > 0) {
                uint32_t chunk = rem > sizeof(buf) ? (uint32_t)sizeof(buf) : (uint32_t)rem;
                if (!clip_recv(c, buf, chunk)) { if (f) fclose(f); return; }
                if (f) fwrite(buf, 1, chunk, f);
                rem -= chunk;
            }
            if (f) fclose(f);
        }
    }
    for (NSString *t in tops) [urls addObject:[NSURL fileURLWithPath:[root stringByAppendingPathComponent:t]]];
    clog("ch6 recv_files: %lu top-level under %s", (unsigned long)tops.count, root.fileSystemRepresentation);
}

static void clip_fetch_and_apply(ClipConn *c, uint32_t fmt, NSString *pbType,
                                 NSMutableDictionary *items, NSMutableArray *urls)
{
    ClipHeader req = { CLIP_MAGIC, CLIP_MSG_FORMAT_DATA_REQ, fmt, 0 };
    if (!clip_send(c, &req, sizeof(req))) return;
    if (fmt == WF_HDROP) { clip_recv_files(c, urls); return; }
    ClipHeader h;
    if (!clip_recv(c, &h, sizeof(h)) || h.msg_type != CLIP_MSG_FORMAT_DATA_RESP) { clog("ch6 fetch fmt=%u: bad resp", fmt); return; }
    uint8_t *data = h.data_size ? malloc(h.data_size) : NULL;
    if (h.data_size && !clip_recv(c, data, h.data_size)) { free(data); return; }
    NSData *raw = data ? [NSData dataWithBytes:data length:h.data_size] : nil;
    free(data);
    clog("ch6 fetch fmt=%u resp=%u bytes", fmt, h.data_size);
    if (!raw.length && fmt != WF_HDROP) return;
    NSData *conv = nil;
    if (fmt == WF_TEXT) {
        NSString *s = [[NSString alloc] initWithData:raw encoding:NSUTF16LittleEndianStringEncoding];
        if (!s) return;
        NSRange z = [s rangeOfString:@"\0"];
        if (z.location != NSNotFound) s = [s substringToIndex:z.location];
        NSData *u8 = [s dataUsingEncoding:NSUTF8StringEncoding];
        if (u8) items[pbType] = u8;
        return;
    } else if (fmt == WF_HTML)  conv = cfhtml_to_html(raw.bytes, (uint32_t)raw.length);
    else if (fmt == WF_DIB)     conv = dib_to_png(raw.bytes, (uint32_t)raw.length);
    else                        conv = raw;   /* RTF passthrough */
    if (conv.length) items[pbType] = conv;
}

static void *clip_reader_thread(void *arg)
{
    (void)arg;
    ClipConn *c = &g_c6;
    for (;;) {
        if (!c->state) { usleep(100000); continue; }
        if (clip_wait_ready(c)) {
            for (;;) {
                ClipHeader h;
                if (!clip_recv(c, &h, sizeof(h))) break;
                if (h.magic != CLIP_MAGIC) break;
                if (h.msg_type != CLIP_MSG_FORMAT_LIST) {                 /* drain stray payloads */
                    if (h.data_size) { uint8_t *s = malloc(h.data_size); clip_recv(c, s, h.data_size); free(s); }
                    continue;
                }
                uint8_t *buf = malloc(h.data_size ? h.data_size : 1);
                if (!buf || !clip_recv(c, buf, h.data_size)) { free(buf); break; }
                /* parse available (id,name) and decide what to fetch */
                BOOL hT=NO,hH=NO,hR=NO,hI=NO,hF=NO;
                uint32_t cnt = h.data_size >= 4 ? *(uint32_t *)buf : 0, o = 4;
                for (uint32_t i = 0; i < cnt && o + 8 <= h.data_size; i++) {
                    uint32_t fmt = *(uint32_t *)(buf + o); o += 4;
                    uint32_t nl = *(uint32_t *)(buf + o); o += 4;
                    NSString *name = nl ? [[NSString alloc] initWithBytes:buf + o length:nl encoding:NSASCIIStringEncoding] : nil;
                    o += nl;
                    if (fmt == WF_TEXT) hT = YES;
                    else if (fmt == WF_HDROP) hF = YES;
                    else if (fmt == WF_DIB) hI = YES;
                    else if ([name isEqualToString:@"HTML Format"]) hH = YES;
                    else if ([name isEqualToString:@"Rich Text Format"]) hR = YES;
                    else if ([name isEqualToString:@"PNG"]) hI = YES;
                }
                free(buf);
                clog("ch6 FORMAT_LIST cnt=%u  text=%d rtf=%d html=%d img=%d files=%d", cnt, hT, hR, hH, hI, hF);
                NSMutableDictionary *items = [NSMutableDictionary dictionary];
                NSMutableArray *urls = [NSMutableArray array];
                if (hF) clip_fetch_and_apply(c, WF_HDROP, nil, items, urls);
                if (hT) clip_fetch_and_apply(c, WF_TEXT, NSPasteboardTypeString, items, urls);
                if (hR) clip_fetch_and_apply(c, WF_RTF, NSPasteboardTypeRTF, items, urls);
                if (hH) clip_fetch_and_apply(c, WF_HTML, NSPasteboardTypeHTML, items, urls);
                if (hI) clip_fetch_and_apply(c, WF_DIB, NSPasteboardTypePNG, items, urls);
                if (items.count || urls.count) {
                    NSDictionary *itemsCopy = [items copy];
                    NSArray *urlsCopy = [urls copy];
                    dispatch_sync(dispatch_get_main_queue(), ^{      /* NSPasteboard writes belong on the main thread */
                        NSPasteboard *pb = [NSPasteboard generalPasteboard];
                        [pb clearContents];
                        BOOL wrote = urlsCopy.count ? [pb writeObjects:urlsCopy] : YES;
                        for (NSString *t in itemsCopy) [pb setData:itemsCopy[t] forType:t];
                        g_clipSuppress = [pb changeCount];
                        clog("ch6 apply (main): items=%lu urls=%lu writeObjects=%d newCC=%ld",
                             (unsigned long)itemsCopy.count, (unsigned long)urlsCopy.count, wrote, g_clipSuppress);
                    });
                } else {
                    clog("ch6 apply: nothing to set");
                }
            }
        }
        clip_rearm(c);
        usleep(50000);
    }
    return NULL;
}

/* ===== ch5: Mac -> Windows (copy-on-Mac) ===== */
static void clip_send_file_entry(ClipConn *c, NSString *full, NSString *rel)
{
    NSFileManager *fm = [NSFileManager defaultManager];
    BOOL isDir = NO;
    if (![fm fileExistsAtPath:full isDirectory:&isDir]) return;
    NSData *relU16 = [[rel stringByReplacingOccurrencesOfString:@"/" withString:@"\\"]
                       dataUsingEncoding:NSUTF16LittleEndianStringEncoding];
    ClipFileInfo fi = { (uint32_t)relU16.length, 0, isDir ? 1 : 0 };
    if (isDir) {
        ClipHeader h = { CLIP_MAGIC, CLIP_MSG_FILE_DATA, 0, (uint32_t)(sizeof(fi) + relU16.length) };
        clip_send(c, &h, sizeof(h)); clip_send(c, &fi, sizeof(fi)); clip_send(c, relU16.bytes, (int)relU16.length);
        for (NSString *child in [fm contentsOfDirectoryAtPath:full error:nil])
            clip_send_file_entry(c, [full stringByAppendingPathComponent:child],
                                 [rel stringByAppendingPathComponent:child]);
    } else {
        NSData *fdata = [NSData dataWithContentsOfFile:full];
        fi.file_size = fdata.length;
        ClipHeader h = { CLIP_MAGIC, CLIP_MSG_FILE_DATA, 0, (uint32_t)(sizeof(fi) + relU16.length) };
        clip_send(c, &h, sizeof(h)); clip_send(c, &fi, sizeof(fi)); clip_send(c, relU16.bytes, (int)relU16.length);
        if (fdata.length) clip_send(c, fdata.bytes, (int)fdata.length);
    }
}
static void clip_serve(ClipConn *c, uint32_t fmt)
{
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    NSData *out = nil;
    if (fmt == WF_TEXT) {
        NSString *s = [pb stringForType:NSPasteboardTypeString];
        if (s) { NSMutableData *m = [[s dataUsingEncoding:NSUTF16LittleEndianStringEncoding] mutableCopy];
                 uint16_t z = 0; [m appendBytes:&z length:2]; out = m; }
    } else if (fmt == WF_RTF)  out = [pb dataForType:NSPasteboardTypeRTF];
    else if (fmt == WF_HTML)  { NSData *h = [pb dataForType:NSPasteboardTypeHTML]; if (h) out = html_to_cfhtml(h); }
    else if (fmt == WF_DIB)   { NSData *img = [pb dataForType:NSPasteboardTypePNG] ?: [pb dataForType:NSPasteboardTypeTIFF];
                                if (img) out = image_to_dib(img); }
    else if (fmt == WF_HDROP) {
        NSArray<NSURL *> *urls = [pb readObjectsForClasses:@[[NSURL class]]
                                   options:@{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }];
        for (NSURL *u in urls) clip_send_file_entry(c, u.path, u.path.lastPathComponent);
        ClipHeader term = { CLIP_MAGIC, CLIP_MSG_FORMAT_DATA_RESP, WF_HDROP, 0 };
        clip_send(c, &term, sizeof(term));
        return;
    }
    ClipHeader h = { CLIP_MAGIC, CLIP_MSG_FORMAT_DATA_RESP, fmt, (uint32_t)out.length };
    clip_send(c, &h, sizeof(h));
    if (out.length) clip_send(c, out.bytes, (int)out.length);
    clog("ch5 serve fmt=%u -> %lu bytes", fmt, (unsigned long)out.length);
}
static void clip_send_format_list(ClipConn *c)
{
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    uint8_t buf[256]; uint32_t o = 4, cnt = 0;
    NSArray *types = pb.types;
    BOOL hasFiles = [pb readObjectsForClasses:@[[NSURL class]]
                      options:@{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }].count > 0;
    #define ADD_FMT(fid, nm) do { const char *_n = (nm); uint32_t _nl = _n ? (uint32_t)strlen(_n) : 0; \
        if (o + 8 + _nl <= sizeof(buf)) { *(uint32_t *)(buf + o) = (fid); o += 4; \
            *(uint32_t *)(buf + o) = _nl; o += 4; if (_nl) { memcpy(buf + o, _n, _nl); o += _nl; } cnt++; } } while (0)
    if (hasFiles)                                      ADD_FMT(WF_HDROP, NULL);
    if ([types containsObject:NSPasteboardTypeString]) ADD_FMT(WF_TEXT, NULL);
    if ([types containsObject:NSPasteboardTypeRTF])    ADD_FMT(WF_RTF, "Rich Text Format");
    if ([types containsObject:NSPasteboardTypeHTML])   ADD_FMT(WF_HTML, "HTML Format");
    if ([types containsObject:NSPasteboardTypePNG] || [types containsObject:NSPasteboardTypeTIFF]) ADD_FMT(WF_DIB, NULL);
    #undef ADD_FMT
    if (cnt == 0) return;
    *(uint32_t *)buf = cnt;
    ClipHeader h = { CLIP_MAGIC, CLIP_MSG_FORMAT_LIST, 0, o };
    clip_send(c, &h, sizeof(h));
    clip_send(c, buf, o);
    clog("ch5 FORMAT_LIST sent: %u formats", cnt);
}
static void *clip_writer_thread(void *arg)
{
    (void)arg;
    ClipConn *c = &g_c5;
    for (;;) {
        if (!c->state) { usleep(100000); continue; }
        if (clip_wait_ready(c)) {
            ClipHeader se = { CLIP_MAGIC, CLIP_MSG_SYNC_ENABLE, 0, 1 }; uint8_t on = 1;
            clip_send(c, &se, sizeof(se)); clip_send(c, &on, 1);
            long last = [[NSPasteboard generalPasteboard] changeCount];
            for (;;) {
                if (!clip_alive(c)) break;
                long cc = [[NSPasteboard generalPasteboard] changeCount];
                if (cc != last) { last = cc; if (cc != g_clipSuppress) clip_send_format_list(c); }
                if (clip_avail(c) >= sizeof(ClipHeader)) {
                    ClipHeader h;
                    if (!clip_recv(c, &h, sizeof(h))) break;
                    if (h.magic == CLIP_MAGIC && h.msg_type == CLIP_MSG_FORMAT_DATA_REQ)
                        clip_serve(c, h.format);
                }
                usleep(30000);
            }
        }
        clip_rearm(c);
        usleep(50000);
    }
    return NULL;
}

/* ---- Guest hardware cursor as the macOS cursor (NSCursor) ----
   The captured frame never contains the HW cursor; the VDD copies its bitmap+hotspot into the
   shared cursor area and we set it as this view's NSCursor so the OS draws it at the real pointer
   position with the correct hotspot. */
static uint32_t  g_curShapeSeen = 0xFFFFFFFFu;
static uint32_t  g_curVisSeen   = 0xFFFFFFFFu;
static double    g_curScale     = 0.0;
static NSCursor *g_nsCursor     = nil;
static void cur_data_free(void *info, const void *data, size_t size) { (void)info; (void)size; free((void *)data); }

/* Build an NSCursor from the shared cursor record (both QueryHardwareCursor3 types are 32bpp BGRA:
   ALPHA(2)=premultiplied; MASKED_COLOR(1)=BGR with the alpha channel as the AND mask, 0xFF=transparent.
   Mirrors create_cursor_from_bitmap in src/backend_win/vm_display_idd.c). A hidden guest cursor maps
   to a fully transparent cursor so the pointer disappears over the view. */
static NSCursor *buildGuestCursor(AsbCursor *cur, double scale)
{
    if (scale <= 0) scale = 1.0;
    if (!cur->visible) {
        NSImage *blank = [[NSImage alloc] initWithSize:NSMakeSize(1, 1)];
        return [[NSCursor alloc] initWithImage:blank hotSpot:NSZeroPoint];
    }
    uint32_t w = cur->width, h = cur->height, pitch = cur->pitch, type = cur->cursor_type;
    if (!w || !h || !cur->image_size || (type != 1 && type != 2) ||
        pitch < w * 4 || (uint64_t)pitch * h > cur->image_size) return nil;
    const uint8_t *src = (const uint8_t *)cur + sizeof(AsbCursor);
    size_t outsz = (size_t)w * h * 4;
    uint8_t *cpy = malloc(outsz);
    if (!cpy) return nil;
    for (uint32_t row = 0; row < h; row++) {
        const uint8_t *s = src + (size_t)row * pitch;
        uint8_t *d = cpy + (size_t)row * w * 4;
        if (type == 2) {
            memcpy(d, s, (size_t)w * 4);                          /* ALPHA: premultiplied BGRA */
        } else {
            for (uint32_t c = 0; c < w; c++) {                    /* MASKED_COLOR: AND mask in alpha */
                uint8_t B = s[c*4+0], G = s[c*4+1], R = s[c*4+2], A = s[c*4+3];
                if (A == 0) {                                     /* opaque colour */
                    d[c*4+0] = B; d[c*4+1] = G; d[c*4+2] = R; d[c*4+3] = 255;
                } else if (B || G || R) {                         /* XOR/invert pixel (e.g. I-beam strokes) */
                    d[c*4+0] = 0; d[c*4+1] = 0; d[c*4+2] = 0; d[c*4+3] = 255;  /* approximate as opaque black */
                } else {                                          /* transparent */
                    d[c*4+0] = 0; d[c*4+1] = 0; d[c*4+2] = 0; d[c*4+3] = 0;
                }
            }
        }
    }
    CGColorSpaceRef cs2 = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef cdp = CGDataProviderCreateWithData(NULL, cpy, outsz, cur_data_free);
    CGImageRef cg = CGImageCreate(w, h, 8, 32, w * 4, cs2,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, cdp, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(cdp);
    CGColorSpaceRelease(cs2);
    if (!cg) return nil;
    /* Display at content scale (VM px * window/guest ratio). The NSImage is in points; the OS then
       applies the retina backing factor, so the cursor matches the scaled frame on a retina display. */
    NSImage *img = [[NSImage alloc] initWithCGImage:cg size:NSMakeSize(w * scale, h * scale)];
    CGImageRelease(cg);
    return [[NSCursor alloc] initWithImage:img hotSpot:NSMakePoint(cur->xhot * scale, cur->yhot * scale)];
}

/* ---- Frame-rendering + input view ---- */
@interface FrameView : NSView
@property(strong) NSTrackingArea *track;
@end

@implementation FrameView
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }

- (void)updateTrackingAreas
{
    if (self.track) [self removeTrackingArea:self.track];
    self.track = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:(NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect)
               owner:self userInfo:nil];
    [self addTrackingArea:self.track];
    [super updateTrackingAreas];
}

/* Apply the guest HW cursor as this view's NSCursor when its shape/visibility changes. */
- (void)updateGuestCursor
{
    /* Apply the cursor stashed by the ch2 recv thread (built from the VDD wire cursor header).
       Rebuild when the shape/visibility changes or the window scale changes (retina-aware). */
    double scale = (g_fbW ? self.bounds.size.width / (double)g_fbW : 1.0);
    pthread_mutex_lock(&g_curLock);
    uint32_t seq = g_curBlobSeq;
    if ((seq == g_curBlobApplied && fabs(scale - g_curScale) < 0.01) || !g_curBlob) {
        pthread_mutex_unlock(&g_curLock);
        return;
    }
    NSCursor *nc = buildGuestCursor((AsbCursor *)g_curBlob, scale);
    pthread_mutex_unlock(&g_curLock);
    if (nc) {
        g_nsCursor = nc;
        g_curBlobApplied = seq;
        g_curScale = scale;
        [self.window invalidateCursorRectsForView:self];
    }
}
- (void)resetCursorRects
{
    if (g_nsCursor) [self addCursorRect:self.bounds cursor:g_nsCursor];
}

- (void)drawRect:(NSRect)dirtyRect
{
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGRect b = NSRectToCGRect(self.bounds);
    CGContextSetRGBFillColor(ctx, 0.05, 0.05, 0.07, 1.0);
    CGContextFillRect(ctx, b);
    /* Render from g_renderFb — the main-thread copy the timer takes when a new frame arrives,
       so we never race the ch2 recv thread mid-frame. */
    if (!g_renderFb || g_fbW == 0 || g_fbH == 0)
        return;

    int w = (int)g_fbW, h = (int)g_fbH, stride = (int)g_fbStride;
    uint8_t *buf = g_renderFb;

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, buf, (size_t)stride * h, NULL);
    CGImageRef img = CGImageCreate(w, h, 8, 32, stride, cs,
        kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little,
        dp, NULL, false, kCGRenderingIntentDefault);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationLow);
    CGContextDrawImage(ctx, b, img);
    CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    /* The hardware cursor is NOT drawn into the frame — it's applied as the macOS cursor
       (NSCursor) via updateGuestCursor below, so the OS renders it at the real pointer
       position with the correct hotspot. */
}

/* ---- mouse ---- */
- (void)recordMove:(NSEvent *)e
{
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    NSRect b = self.bounds;
    if (b.size.width < 1 || b.size.height < 1) return;
    double nx = p.x / b.size.width;
    double ny = (b.size.height - p.y) / b.size.height;   /* flip y: window is y-up, guest top-down */
    if (nx < 0) nx = 0; if (nx > 1) nx = 1;
    if (ny < 0) ny = 0; if (ny > 1) ny = 1;
    uint32_t gw = g_fbW ? g_fbW : 1920;
    uint32_t gh = g_fbH ? g_fbH : 1080;
    g_moveX = (uint32_t)(nx * (gw - 1));
    g_moveY = (uint32_t)(ny * (gh - 1));
    g_hasMove = 1;                                        /* coalesced; flushed on tick / before discrete events */
}
- (void)mouseMoved:(NSEvent *)e        { [self recordMove:e]; }
- (void)mouseDragged:(NSEvent *)e      { [self recordMove:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self recordMove:e]; }
- (void)otherMouseDragged:(NSEvent *)e { [self recordMove:e]; }
- (void)mouseDown:(NSEvent *)e  { [self recordMove:e]; flush_move(); send_input(INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 1, 0); }
- (void)mouseUp:(NSEvent *)e    { flush_move(); send_input(INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 0, 0); }
- (void)rightMouseDown:(NSEvent *)e { [self recordMove:e]; flush_move(); send_input(INPUT_MOUSE_BUTTON, INPUT_BTN_RIGHT, 1, 0); }
- (void)rightMouseUp:(NSEvent *)e   { flush_move(); send_input(INPUT_MOUSE_BUTTON, INPUT_BTN_RIGHT, 0, 0); }
- (void)otherMouseDown:(NSEvent *)e { if (e.buttonNumber == 2) { [self recordMove:e]; flush_move(); send_input(INPUT_MOUSE_BUTTON, INPUT_BTN_MIDDLE, 1, 0); } }
- (void)otherMouseUp:(NSEvent *)e   { if (e.buttonNumber == 2) { flush_move(); send_input(INPUT_MOUSE_BUTTON, INPUT_BTN_MIDDLE, 0, 0); } }
- (void)scrollWheel:(NSEvent *)e
{
    flush_move();
    double dy = e.hasPreciseScrollingDeltas ? e.scrollingDeltaY : e.scrollingDeltaY * 10.0;
    int32_t delta = (int32_t)(dy * 12.0);
    if (delta != 0) send_input(INPUT_MOUSE_WHEEL, (uint32_t)delta, 0, 0);
}

/* ---- keyboard ---- */
- (void)keyDown:(NSEvent *)e
{
    flush_move();
    unsigned short kc = e.keyCode;
    uint8_t vk = (kc < 128) ? g_vk[kc] : 0;
    if (vk) send_input(INPUT_KEY, vk, 0, is_extended(kc) ? 1 : 0);
    /* swallow (no super) to avoid the system beep */
}
- (void)keyUp:(NSEvent *)e
{
    flush_move();
    unsigned short kc = e.keyCode;
    uint8_t vk = (kc < 128) ? g_vk[kc] : 0;
    if (vk) send_input(INPUT_KEY, vk, 0, (is_extended(kc) ? 1 : 0) | 2);
}
- (void)flagsChanged:(NSEvent *)e
{
    flush_move();
    unsigned short kc = e.keyCode;
    uint32_t vk = 0, mask = 0;
    switch (kc) {
    case 0x38: case 0x3C: vk = 0x10; mask = NSEventModifierFlagShift;    break; /* Shift */
    case 0x3B: case 0x3E: vk = 0x11; mask = NSEventModifierFlagControl;  break; /* Control */
    case 0x3A: case 0x3D: vk = 0x12; mask = NSEventModifierFlagOption;   break; /* Option->Alt */
    case 0x37:            vk = 0x5B; mask = NSEventModifierFlagCommand;  break; /* LCmd->LWin */
    case 0x36:            vk = 0x5C; mask = NSEventModifierFlagCommand;  break; /* RCmd->RWin */
    case 0x39:            vk = 0x14; mask = NSEventModifierFlagCapsLock; break; /* CapsLock */
    default: return;
    }
    BOOL down = (e.modifierFlags & mask) != 0;
    send_input(INPUT_KEY, vk, 0, down ? 0 : 2);
}
@end

/* ---- App delegate ---- */
@interface AppDelegate : NSObject <NSApplicationDelegate>
@property(strong) NSWindow *window;
@property(strong) FrameView *view;
@property(strong) NSTimer *timer;
@end

@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    NSRect frame = NSMakeRect(0, 0, 1280, 720);
    self.window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered defer:NO];
    [self.window setTitle:@"AppSandbox VM — ivshmem (display + input)"];
    [self.window setAcceptsMouseMovedEvents:YES];
    [self.window center];

    self.view = [[FrameView alloc] initWithFrame:frame];
    [self.window setContentView:self.view];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.view];
    [NSApp activateIgnoringOtherApps:YES];

    self.timer = [NSTimer scheduledTimerWithTimeInterval:(1.0 / 60.0) repeats:YES block:^(NSTimer *t) {
        flush_move();   /* emit at most one coalesced mouse move per tick */
        [self.view updateGuestCursor];   /* mirror the guest HW cursor onto the macOS cursor */
        /* If the ch2 recv thread applied a new frame, copy it into the render buffer (under lock,
           so we never read a half-written frame) and redraw. */
        if (g_fbSeq != g_renderSeq && g_fb) {
            pthread_mutex_lock(&g_fbLock);
            size_t need = (size_t)g_fbStride * g_fbH;
            if (need) {
                if (!g_renderFb) g_renderFb = malloc(need);
                if (g_renderFb) memcpy(g_renderFb, g_fb, need);
            }
            g_renderSeq = g_fbSeq;
            pthread_mutex_unlock(&g_fbLock);
            [self.view setNeedsDisplay:YES];
        }
    }];

    pthread_t dispThread;
    pthread_create(&dispThread, NULL, display_recv_thread, NULL);  /* ch2: VDD frame stream */
    pthread_detach(dispThread);
    pthread_t audioThread, clipR, clipW;
    pthread_create(&audioThread, NULL, audio_reader_thread, NULL);
    pthread_detach(audioThread);
    pthread_create(&clipR, NULL, clip_reader_thread, NULL);   /* ch6: Windows -> Mac */
    pthread_detach(clipR);
    pthread_create(&clipW, NULL, clip_writer_thread, NULL);   /* ch5: Mac -> Windows */
    pthread_detach(clipW);
    pthread_t agentT;
    pthread_create(&agentT, NULL, agent_control_thread, NULL);  /* ch1: agent control */
    pthread_detach(agentT);
    pthread_t sshT;
    pthread_create(&sshT, NULL, ssh_listener_thread, NULL);     /* ch7: SSH proxy (TCP -> ivshmem) */
    pthread_detach(sshT);
    pthread_t p9T;
    pthread_create(&p9T, NULL, p9_server_thread, NULL);         /* ch9P: read-only 9P share */
    pthread_detach(p9T);
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)s { return YES; }
@end

/* ================================================================================
 * ch1 AGENT control connector — proves agent.c's ch1 wiring works over ivshmem.
 * The agent sends "hello" on connect, then "heartbeat" every ~5s plus "log:" / "idd:" lines.
 * We complete the handshake (arm CONNECTING -> agent's asb_accept makes it ESTABLISHED),
 * log every line, and send one "1:ping" to verify the request/reply path (agent -> "1:ok").
 * ================================================================================ */
static void aclog(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[agent-ch1] "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); fflush(stderr);
    va_end(ap);
}

static void *agent_control_thread(void *arg)
{
    (void)arg;
    char line[1024];
    for (;;) {
        if (!g_c1.state) { usleep(100000); continue; }
        /* wait for the agent to accept */
        if (*g_c1.state != ASB_SLOT_ESTABLISHED) { usleep(50000); continue; }
        aclog("connected (slot ESTABLISHED). Sending ping + ssh_enable...");
        ring_write_host(g_c1.h2g, g_c1.h2gData, "1:ping\n", 7);
        /* Ask the guest agent to ensure sshd is running and start its ch7 ssh proxy. The agent
           replies ssh_ready / ssh_installing / ssh_failed (logged below). */
        ring_write_host(g_c1.h2g, g_c1.h2gData, "2:ssh_enable\n", 13);

        int pos = 0;
        for (;;) {
            uint32_t st = *g_c1.state;
            if (st == ASB_SLOT_CLOSING || st == ASB_SLOT_CLOSED) break;
            char c;
            int n = ring_read_host(g_c1.g2h, g_c1.g2hData, &c, 1);
            if (n <= 0) { usleep(2000); continue; }
            if (c == '\r') continue;
            if (c == '\n') { line[pos] = '\0'; if (pos) aclog("recv: %s", line); pos = 0; continue; }
            if (pos < (int)sizeof(line) - 1) line[pos++] = c;
        }
        aclog("slot closed; re-arming.");
        g_c1.g2h->head = g_c1.g2h->tail = 0;
        g_c1.h2g->head = g_c1.h2g->tail = 0;
        __sync_synchronize();
        *g_c1.state = ASB_SLOT_CONNECTING;
        usleep(50000);
    }
    return NULL;
}

/* ================================================================================
 * ch2 DISPLAY connector — reconstructs the VDD wire stream into g_fb. We are the connector
 * (arm CONNECTING); the guest VDD's asb_accept claims the slot and, on each fresh accept, sends a
 * full frame first (bSentFullFrame=FALSE) then dirty-rect frames + cursor updates. Mirrors the recv
 * loop in src/backend_win/vm_display_idd.c. On slot close we re-arm so the next VDD swap chain
 * reconnects (a new accept => a fresh full frame), exactly like reopening the IDD window on Windows.
 * ================================================================================ */
static void dlog(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[display] "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); fflush(stderr);
    va_end(ap);
}

static void *display_recv_thread(void *arg)
{
    (void)arg;
    uint8_t *scratch = NULL;            /* reused dirty-rect row staging */
    size_t   scratchSz = 0;
    for (;;) {
        if (!g_c2.state) { usleep(100000); continue; }
        if (*g_c2.state != ASB_SLOT_ESTABLISHED) { usleep(20000); continue; }
        dlog("connected (slot ESTABLISHED); awaiting frames.");

        for (;;) {
            uint32_t magic;
            if (!clip_recv(&g_c2, &magic, 4)) break;

            if (magic == VDD_CURSOR_MAGIC) {
                WireCursorHeader ch;
                ch.magic = magic;
                if (!clip_recv(&g_c2, (uint8_t *)&ch + 4, sizeof(ch) - 4)) break;
                uint8_t *shape = NULL;
                if (ch.shape_updated && ch.shape_data_size > 0) {
                    if (ch.shape_data_size > 4u*1024*1024) break;   /* sanity */
                    shape = malloc(ch.shape_data_size);
                    if (!shape) break;
                    if (!clip_recv(&g_c2, shape, (int)ch.shape_data_size)) { free(shape); break; }
                }
                /* Build an AsbCursor blob (header + image) so the render timer can reuse
                   buildGuestCursor unchanged. shape_updated=0 => keep last image, update visibility. */
                pthread_mutex_lock(&g_curLock);
                if (shape) {
                    uint8_t *blob = malloc(sizeof(AsbCursor) + ch.shape_data_size);
                    if (blob) {
                        AsbCursor *c = (AsbCursor *)blob;
                        memset(c, 0, sizeof(*c));
                        c->magic = ASB_CURSOR_MAGIC;
                        c->visible = ch.visible; c->width = ch.width; c->height = ch.height;
                        c->pitch = ch.pitch; c->xhot = ch.xhot; c->yhot = ch.yhot;
                        c->cursor_type = ch.cursor_type; c->image_size = ch.shape_data_size;
                        memcpy(blob + sizeof(AsbCursor), shape, ch.shape_data_size);
                        free(g_curBlob); g_curBlob = blob; g_curBlobSeq++;
                    }
                } else if (g_curBlob) {
                    AsbCursor *c = (AsbCursor *)g_curBlob;
                    if (c->visible != ch.visible) { c->visible = ch.visible; g_curBlobSeq++; }
                }
                pthread_mutex_unlock(&g_curLock);
                free(shape);
                continue;
            }

            if (magic != VDD_FRAME_MAGIC) { dlog("bad magic 0x%08x; reconnecting.", magic); break; }

            WireFrameHeader fh;
            fh.magic = magic;
            if (!clip_recv(&g_c2, (uint8_t *)&fh + 4, sizeof(fh) - 4)) break;
            if (fh.width == 0 || fh.height == 0 || fh.stride < fh.width * 4 ||
                fh.width > 8192 || fh.height > 8192) { dlog("bad frame dims; reconnecting."); break; }

            /* (Re)allocate the working framebuffer if geometry changed. */
            if (g_fbW != fh.width || g_fbH != fh.height || g_fbStride != fh.stride || !g_fb) {
                uint8_t *nb = malloc((size_t)fh.stride * fh.height);
                if (!nb) break;
                pthread_mutex_lock(&g_fbLock);
                free(g_fb); g_fb = nb;
                g_fbW = fh.width; g_fbH = fh.height; g_fbStride = fh.stride;
                memset(g_fb, 0, (size_t)g_fbStride * g_fbH);
                pthread_mutex_unlock(&g_fbLock);
            }

            if (fh.dirty_rect_count == 0) {
                /* Full frame: data_size then height*stride bytes straight into g_fb. */
                uint32_t data_size = 0;
                if (!clip_recv(&g_c2, &data_size, 4)) break;
                uint32_t want = g_fbStride * g_fbH;
                if (data_size != want) { dlog("full-frame size %u != %u; reconnecting.", data_size, want); break; }
                pthread_mutex_lock(&g_fbLock);
                BOOL ok = clip_recv(&g_c2, g_fb, (int)data_size);
                if (ok) g_fbSeq++;
                pthread_mutex_unlock(&g_fbLock);
                if (!ok) break;
            } else {
                /* Dirty rects: rects[], data_size, then per-rect packed rows. */
                if (fh.dirty_rect_count > VDD_MAX_DIRTY) { dlog("rect count %u too big.", fh.dirty_rect_count); break; }
                WireRect rects[VDD_MAX_DIRTY];
                if (!clip_recv(&g_c2, rects, (int)(fh.dirty_rect_count * sizeof(WireRect)))) break;
                uint32_t data_size = 0;
                if (!clip_recv(&g_c2, &data_size, 4)) break;
                if (data_size > scratchSz) { free(scratch); scratch = malloc(data_size); scratchSz = data_size; if (!scratch) break; }
                if (!clip_recv(&g_c2, scratch, (int)data_size)) break;
                /* Patch each rect's rows into g_fb. */
                pthread_mutex_lock(&g_fbLock);
                size_t off = 0;
                for (uint32_t i = 0; i < fh.dirty_rect_count; i++) {
                    int left = rects[i].left, top = rects[i].top, right = rects[i].right, bot = rects[i].bottom;
                    if (left < 0) left = 0; if (top < 0) top = 0;
                    if (right > (int)g_fbW) right = (int)g_fbW;
                    if (bot > (int)g_fbH) bot = (int)g_fbH;
                    if (left >= right || top >= bot) continue;
                    uint32_t rw = (uint32_t)(right - left), rh = (uint32_t)(bot - top);
                    for (uint32_t row = 0; row < rh; row++) {
                        if (off + rw * 4 > data_size) { i = fh.dirty_rect_count; break; }
                        memcpy(g_fb + (size_t)(top + row) * g_fbStride + (size_t)left * 4,
                               scratch + off, rw * 4);
                        off += rw * 4;
                    }
                }
                g_fbSeq++;
                pthread_mutex_unlock(&g_fbLock);
            }
        }

        dlog("slot closed; re-arming for next VDD connection.");
        g_c2.g2h->head = g_c2.g2h->tail = 0;
        g_c2.h2g->head = g_c2.h2g->tail = 0;
        __sync_synchronize();
        *g_c2.state = ASB_SLOT_CONNECTING;
        usleep(50000);
    }
    free(scratch);
    return NULL;
}

/* ================================================================================
 * ch7 SSH proxy (host side) — the ivshmem analog of src/backend_mac/vm_ssh_proxy_mac.m's vsock
 * bridge. A loopback TCP listener; each accepted connection claims a ch7 slot, arms CONNECTING, and
 * relays bytes to/from the guest agent's ssh_proxy (which forwards to the guest's localhost:22). So
 * `ssh -p <port> user@127.0.0.1` reaches the guest sshd with NO guest networking.
 * Host is the connector: host->guest = h2g ring, guest->host = g2h ring.
 * ================================================================================ */
static pthread_mutex_t g_sshLock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int tcp_fd;
    volatile uint32_t *state;
    AsbRing *g2h; uint8_t *g2hData;   /* guest->host */
    AsbRing *h2g; uint8_t *h2gData;   /* host->guest */
} SshRelay;

static int ssh_send_all_tcp(int fd, const void *buf, int len)
{
    int off = 0;
    while (off < len) {
        ssize_t w = send(fd, (const char *)buf + off, (size_t)(len - off), 0);
        if (w <= 0) return -1;
        off += (int)w;
    }
    return 0;
}

static void *ssh_relay_thread(void *arg)
{
    SshRelay *r = (SshRelay *)arg;
    char buf[8192];
    for (;;) {
        if (*r->state != ASB_SLOT_ESTABLISHED) break;        /* guest closed its end */
        /* guest -> host: drain g2h, forward to the ssh client */
        int n = ring_read_host(r->g2h, r->g2hData, buf, (int)sizeof(buf));
        if (n > 0) { if (ssh_send_all_tcp(r->tcp_fd, buf, n) != 0) break; continue; }
        /* host -> guest: is the ssh client readable? (2 ms so guest->host stays responsive) */
        fd_set rfds; FD_ZERO(&rfds); FD_SET(r->tcp_fd, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 2000 };
        int s = select(r->tcp_fd + 1, &rfds, NULL, NULL, &tv);
        if (s < 0) break;
        if (s > 0 && FD_ISSET(r->tcp_fd, &rfds)) {
            ssize_t rd = recv(r->tcp_fd, buf, sizeof(buf), 0);
            if (rd <= 0) break;
            int off = 0;
            while (off < (int)rd) {
                if (*r->state != ASB_SLOT_ESTABLISHED) goto done;
                int w = ring_write_host(r->h2g, r->h2gData, buf + off, (int)rd - off);
                if (w > 0) off += w; else usleep(500);   /* ring full: guest draining */
            }
        }
    }
done:
    close(r->tcp_fd);
    __sync_synchronize();
    *r->state = ASB_SLOT_FREE;   /* guest relay sees != ESTABLISHED -> closes; slot reusable */
    free(r);
    return NULL;
}

static void *ssh_listener_thread(void *arg)
{
    (void)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { fprintf(stderr, "[ssh] socket failed\n"); return NULL; }
    int yes = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(SSH_LISTEN_PORT);
    int port = SSH_LISTEN_PORT;
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0) {   /* port busy -> ephemeral */
        a.sin_port = 0;
        if (bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0) { fprintf(stderr, "[ssh] bind failed\n"); close(ls); return NULL; }
    }
    if (listen(ls, 4) != 0) { fprintf(stderr, "[ssh] listen failed\n"); close(ls); return NULL; }
    { socklen_t al = sizeof(a); if (getsockname(ls, (struct sockaddr *)&a, &al) == 0) port = ntohs(a.sin_port); }
    fprintf(stderr, "[ssh] listening on 127.0.0.1:%d  ->  guest ch7 -> sshd:22  (ssh -p %d user@127.0.0.1)\n", port, port);

    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) continue;

        /* Claim a free ch7 slot and arm CONNECTING. */
        pthread_mutex_lock(&g_sshLock);
        int idx = -1;
        for (uint32_t i = 0; i < SSH_N_SLOTS; i++) {
            volatile uint32_t *st = (volatile uint32_t *)(g_sshRegion + (uint64_t)i * g_sshSlotStride);
            if (*st == ASB_SLOT_FREE || *st == ASB_SLOT_CLOSED) { idx = (int)i; break; }
        }
        if (idx < 0) { pthread_mutex_unlock(&g_sshLock); fprintf(stderr, "[ssh] no free slot\n"); close(c); continue; }
        uint8_t *slot = g_sshRegion + (uint64_t)idx * g_sshSlotStride;
        volatile uint32_t *st = (volatile uint32_t *)slot;
        AsbRing *g2h = (AsbRing *)(slot + ASB_SLOT_HDR);
        uint8_t *g2hd = (uint8_t *)g2h + ASB_RING_HDR;
        AsbRing *h2g = (AsbRing *)(g2hd + SSH_RING_CAP);
        uint8_t *h2gd = (uint8_t *)h2g + ASB_RING_HDR;
        g2h->head = g2h->tail = 0; h2g->head = h2g->tail = 0;
        g2h->cap = SSH_RING_CAP; h2g->cap = SSH_RING_CAP;
        __sync_synchronize();
        *st = ASB_SLOT_CONNECTING;
        pthread_mutex_unlock(&g_sshLock);

        /* Wait for the guest ssh_proxy to accept (needs ssh_enable sent + sshd up). */
        int waited = 0;
        while (*st != ASB_SLOT_ESTABLISHED && waited < 5000) { usleep(2000); waited += 2; }
        if (*st != ASB_SLOT_ESTABLISHED) {
            fprintf(stderr, "[ssh] guest did not accept slot %d (is ssh_enable sent / sshd running?)\n", idx);
            __sync_synchronize(); *st = ASB_SLOT_FREE;
            close(c); continue;
        }

        SshRelay *r = (SshRelay *)malloc(sizeof(*r));
        if (!r) { __sync_synchronize(); *st = ASB_SLOT_FREE; close(c); continue; }
        r->tcp_fd = c; r->state = st;
        r->g2h = g2h; r->g2hData = g2hd; r->h2g = h2g; r->h2gData = h2gd;
        pthread_t t; pthread_create(&t, NULL, ssh_relay_thread, r); pthread_detach(t);
        fprintf(stderr, "[ssh] ssh client connected -> slot %d\n", idx);
    }
}

/* ================================================================================
 * ch9P read-only 9P2000.L server (host side). The guest p9copy is the connector (asb_connect arms a
 * slot CONNECTING); we accept and serve P9_SHARE_ROOT read-only. Implements exactly the messages the
 * p9copy client sends: version/attach/walk/lopen/read/readdir/getattr/clunk. Guest writes T-messages
 * to g2h; we read them and write R-messages to h2g.
 * ================================================================================ */
#define P9_MSIZE_SRV   65536u
#define P9_TVERSION 100
#define P9_RVERSION 101
#define P9_TATTACH  104
#define P9_RATTACH  105
#define P9_TWALK    110
#define P9_RWALK    111
#define P9_TREAD    116
#define P9_RREAD    117
#define P9_TCLUNK   120
#define P9_RCLUNK   121
#define P9_RLERROR    7
#define P9_TLOPEN    12
#define P9_RLOPEN    13
#define P9_TGETATTR  24
#define P9_RGETATTR  25
#define P9_TREADDIR  40
#define P9_RREADDIR  41
#define P9_QTDIR   0x80

typedef struct { int used; uint32_t fid; char path[1024]; } P9Fid;
#define P9_MAXFID 4096

static uint16_t p9g16(const uint8_t *p){ return (uint16_t)(p[0]|(p[1]<<8)); }
static uint32_t p9g32(const uint8_t *p){ return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint64_t p9g64(const uint8_t *p){ uint64_t v=0; for(int i=0;i<8;i++) v|=(uint64_t)p[i]<<(8*i); return v; }
static void p9p16(uint8_t **o, uint16_t v){ (*o)[0]=v; (*o)[1]=v>>8; *o+=2; }
static void p9p32(uint8_t **o, uint32_t v){ for(int i=0;i<4;i++)(*o)[i]=v>>(8*i); *o+=4; }
static void p9p64(uint8_t **o, uint64_t v){ for(int i=0;i<8;i++)(*o)[i]=v>>(8*i); *o+=8; }
static void p9pstr(uint8_t **o, const char *s){ uint16_t n=(uint16_t)strlen(s); p9p16(o,n); memcpy(*o,s,n); *o+=n; }
static void p9pqid(uint8_t **o, const struct stat *st){
    *(*o)++ = S_ISDIR(st->st_mode) ? P9_QTDIR : 0;     /* type */
    p9p32(o, 0);                                       /* version */
    p9p64(o, (uint64_t)st->st_ino);                    /* path */
}

static P9Fid *p9_fid_find(P9Fid *t, uint32_t fid){ for(int i=0;i<P9_MAXFID;i++) if(t[i].used&&t[i].fid==fid) return &t[i]; return NULL; }
static P9Fid *p9_fid_alloc(P9Fid *t, uint32_t fid){ P9Fid*f=p9_fid_find(t,fid); if(f) return f; for(int i=0;i<P9_MAXFID;i++) if(!t[i].used){ t[i].used=1; t[i].fid=fid; return &t[i]; } return NULL; }

/* Reject path components that could escape the share. */
static int p9_name_ok(const char *n){ return n[0] && strcmp(n,".") && strcmp(n,"..") && !strchr(n,'/') && !strchr(n,'\\'); }

/* Blocking read of exactly len bytes of a 9P message off the slot's g2h ring. */
static int p9_read_exact(ClipConn *c, void *buf, int len){
    int got=0;
    while(got<len){
        if(*c->state != ASB_SLOT_ESTABLISHED) return 0;
        int n=ring_read_host(c->g2h, c->g2hData, (uint8_t*)buf+got, len-got);
        if(n>0) got+=n; else usleep(300);
    }
    return 1;
}
static int p9_write_all(ClipConn *c, const void *buf, int len){
    int off=0;
    while(off<len){
        if(*c->state != ASB_SLOT_ESTABLISHED) return 0;
        int n=ring_write_host(c->h2g, c->h2gData, (const uint8_t*)buf+off, len-off);
        if(n>0) off+=n; else usleep(300);
    }
    return 1;
}

/* Serve one 9P connection on an established ch9P slot. */
static void p9_serve(ClipConn *c){
    P9Fid *fids = calloc(P9_MAXFID, sizeof(P9Fid));
    uint8_t *rx = malloc(P9_MSIZE_SRV), *tx = malloc(P9_MSIZE_SRV);
    uint32_t msize = P9_MSIZE_SRV;
    if(!fids||!rx||!tx){ free(fids); free(rx); free(tx); return; }
    for(;;){
        uint8_t hdr[4];
        if(!p9_read_exact(c, hdr, 4)) break;
        uint32_t sz = p9g32(hdr);
        if(sz<7 || sz>P9_MSIZE_SRV) break;
        memcpy(rx, hdr, 4);
        if(!p9_read_exact(c, rx+4, sz-4)) break;
        uint8_t  type = rx[4];
        uint16_t tag  = p9g16(rx+5);
        const uint8_t *p = rx+7;
        uint8_t *o = tx+7;          /* response payload cursor (after size[4] type[1] tag[2]) */
        uint8_t rtype = P9_RLERROR;
        uint32_t err = 0;

        if(type==P9_TVERSION){
            uint32_t cms = p9g32(p); char ver[32]={0};
            uint16_t vl=p9g16(p+4); if(vl>31)vl=31; memcpy(ver,p+6,vl);
            msize = cms<P9_MSIZE_SRV ? cms : P9_MSIZE_SRV; if(msize<256) msize=256;
            rtype=P9_RVERSION; p9p32(&o,msize);
            p9pstr(&o, strcmp(ver,"9P2000.L")==0 ? "9P2000.L" : "unknown");
        }
        else if(type==P9_TATTACH){
            uint32_t fid=p9g32(p);
            P9Fid *f=p9_fid_alloc(fids,fid);
            struct stat st;
            if(f && stat(P9_SHARE_ROOT,&st)==0){ snprintf(f->path,sizeof f->path,"%s",P9_SHARE_ROOT);
                rtype=P9_RATTACH; p9pqid(&o,&st); }
            else err=2 /*ENOENT*/;
        }
        else if(type==P9_TWALK){
            uint32_t fid=p9g32(p), newfid=p9g32(p+4); uint16_t nw=p9g16(p+8);
            const uint8_t *wp=p+10;
            P9Fid *f=p9_fid_find(fids,fid);
            if(!f){ err=9 /*EBADF*/; }
            else {
                char cur[1024]; snprintf(cur,sizeof cur,"%s",f->path);
                uint8_t *nwqid_at=o; p9p16(&o,0); uint16_t got=0; int ok=1;
                for(uint16_t i=0;i<nw;i++){
                    uint16_t nl=p9g16(wp); char nm[256]={0}; if(nl>255)nl=255; memcpy(nm,wp+2,nl); wp+=2+p9g16(wp);
                    if(!p9_name_ok(nm)){ ok=0; break; }
                    char next[1024]; snprintf(next,sizeof next,"%s/%s",cur,nm);
                    struct stat st; if(stat(next,&st)!=0){ ok=0; break; }
                    snprintf(cur,sizeof cur,"%s",next);
                    p9pqid(&o,&st); got++;
                }
                if(!ok && got==0){ o=tx+7; err=2; }    /* first component failed -> error */
                else { uint8_t *sav=o; o=nwqid_at; p9p16(&o,got); o=sav; rtype=P9_RWALK;
                       P9Fid *nf=p9_fid_alloc(fids,newfid); if(nf) snprintf(nf->path,sizeof nf->path,"%s",cur); }
            }
        }
        else if(type==P9_TLOPEN){
            uint32_t fid=p9g32(p); P9Fid *f=p9_fid_find(fids,fid); struct stat st;
            if(f && stat(f->path,&st)==0){ rtype=P9_RLOPEN; p9pqid(&o,&st); p9p32(&o,0 /*iounit*/); }
            else err=2;
        }
        else if(type==P9_TREAD){
            uint32_t fid=p9g32(p); uint64_t off=p9g64(p+4); uint32_t cnt=p9g32(p+12);
            P9Fid *f=p9_fid_find(fids,fid);
            uint32_t cap = msize-11; if(cnt>cap) cnt=cap;
            uint8_t *datap = o+4;       /* after count[4] */
            int fd = f ? open(f->path, O_RDONLY) : -1;
            if(fd>=0){ ssize_t r=pread(fd,datap,cnt,(off_t)off); close(fd);
                if(r<0)r=0; rtype=P9_RREAD; p9p32(&o,(uint32_t)r); o+=r; }
            else err=2;
        }
        else if(type==P9_TREADDIR){
            uint32_t fid=p9g32(p); uint64_t off=p9g64(p+4); uint32_t cnt=p9g32(p+12);
            P9Fid *f=p9_fid_find(fids,fid);
            uint32_t cap = msize-11; if(cnt>cap) cnt=cap;
            uint8_t *cntp=o; p9p32(&o,0); uint8_t *datap=o; uint32_t used=0;
            DIR *d = f ? opendir(f->path) : NULL;
            if(d){
                uint64_t idx=0; struct dirent *de;
                while((de=readdir(d))){
                    if(idx < off){ idx++; continue; }
                    if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")){ idx++; continue; }
                    char full[1024]; snprintf(full,sizeof full,"%s/%s",f->path,de->d_name); struct stat st;
                    if(stat(full,&st)!=0){ idx++; continue; }
                    uint32_t entlen = 13+8+1+2+(uint32_t)strlen(de->d_name);
                    if(used+entlen>cnt) break;
                    p9pqid(&datap,&st);                              /* qid[13] */
                    p9p64(&datap, idx+1);                            /* next-offset cookie */
                    *datap++ = S_ISDIR(st.st_mode)?4:8;              /* dirent type DT_DIR/DT_REG */
                    p9pstr(&datap, de->d_name);
                    used+=entlen; idx++;
                }
                closedir(d);
                { uint8_t *sav=o; o=cntp; p9p32(&o,used); o=sav+used; }
                rtype=P9_RREADDIR;
            } else err=2;
        }
        else if(type==P9_TGETATTR){
            uint32_t fid=p9g32(p); P9Fid *f=p9_fid_find(fids,fid); struct stat st;
            if(f && stat(f->path,&st)==0){
                rtype=P9_RGETATTR;
                p9p64(&o, 0x000007ffULL);                /* valid mask (basic) */
                p9pqid(&o,&st);                          /* qid[13] */
                p9p32(&o, (uint32_t)st.st_mode);         /* mode */
                p9p32(&o, 0); p9p32(&o, 0);              /* uid, gid */
                p9p64(&o, (uint64_t)st.st_nlink);        /* nlink */
                p9p64(&o, 0);                            /* rdev */
                p9p64(&o, (uint64_t)st.st_size);         /* size */
                p9p64(&o, 4096); p9p64(&o, ((uint64_t)st.st_size+511)/512); /* blksize, blocks */
                for(int i=0;i<8;i++) p9p64(&o, 0);       /* atime/mtime/ctime/btime (sec+nsec) */
                p9p64(&o, 0); p9p64(&o, 0);              /* gen, data_version */
            } else err=2;
        }
        else if(type==P9_TCLUNK){
            uint32_t fid=p9g32(p); P9Fid *f=p9_fid_find(fids,fid); if(f) f->used=0;
            rtype=P9_RCLUNK;
        }
        /* else: leave rtype=RLERROR with err=EOPNOTSUPP */
        else err=95;

        if(rtype==P9_RLERROR){ o=tx+7; p9p32(&o, err?err:95); }
        uint32_t rsz=(uint32_t)(o-tx);
        tx[0]=rsz; tx[1]=rsz>>8; tx[2]=rsz>>16; tx[3]=rsz>>24; tx[4]=rtype; tx[5]=tag; tx[6]=tag>>8;
        if(!p9_write_all(c, tx, rsz)) break;
    }
    free(fids); free(rx); free(tx);
}

static void *p9_server_thread(void *arg){
    (void)arg;
    for(;;){
        for(uint32_t i=0;i<P9_N_SLOTS;i++){
            uint8_t *slot = g_p9Region + (uint64_t)i*g_p9SlotStride;
            volatile uint32_t *st = (volatile uint32_t*)slot;
            if(*st != ASB_SLOT_CONNECTING) continue;
            ClipConn c;
            c.state = st;
            c.g2h = (AsbRing*)(slot+ASB_SLOT_HDR); c.g2hData = (uint8_t*)c.g2h + ASB_RING_HDR;
            c.h2g = (AsbRing*)(c.g2hData + P9_RING_CAP); c.h2gData = (uint8_t*)c.h2g + ASB_RING_HDR;
            __sync_synchronize();
            *st = ASB_SLOT_ESTABLISHED;                 /* accept (mirrors asb_connect handshake) */
            fprintf(stderr, "[9p] client connected on slot %u; serving %s\n", i, P9_SHARE_ROOT);
            p9_serve(&c);
            __sync_synchronize();
            *st = ASB_SLOT_FREE;                        /* release for reuse */
            fprintf(stderr, "[9p] client disconnected (slot %u)\n", i);
        }
        usleep(2000);
    }
    return NULL;
}

int main(int argc, const char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/tmp/ivshmem.bin";
    build_keymap();
    if (map_and_publish(path) != 0) return 1;
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDelegate *del = [[AppDelegate alloc] init];
        [app setDelegate:del];
        [app run];
    }
    return 0;
}
