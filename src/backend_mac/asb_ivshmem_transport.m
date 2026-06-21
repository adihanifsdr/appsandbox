#import "asb_ivshmem_transport.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>

#include "../../tools/transport/asb_transport.h"   /* AsbShmDirectory, AsbRing, ASB_SLOT_*, ASB_SLOT_HDR */

#define ASB_RING_HDR ((uint32_t)sizeof(AsbRing))
#define MAX_PUMPS    32

/* ---- SPSC ring helpers (host side). Host is consumer of g2h, producer of h2g.
 * Identical math to asb_transport.c's ring_read_host/ring_write_host. ---- */
static int ring_read_host(AsbRing *r, uint8_t *data, void *buf, int len)
{
    __sync_synchronize();
    uint64_t head = r->head, tail = r->tail; uint32_t cap = r->cap;
    uint32_t used = (uint32_t)(tail - head);
    uint32_t n = (uint32_t)len < used ? (uint32_t)len : used;
    if (n == 0) return 0;
    __sync_synchronize();   /* ACQUIRE: order the `tail` load before the data read. Without it, weak
                             * memory (ARM64) lets the read be speculated ahead of the tail load and
                             * return bytes the producer hasn't written yet, corrupting large transfers.
                             * See the matching fix + rationale in ring_read() (tools/transport/asb_transport.c). */
    uint32_t first = cap - (uint32_t)(head & (cap - 1));
    if (first >= n) memcpy(buf, data + (head & (cap - 1)), n);
    else { memcpy(buf, data + (head & (cap - 1)), first); memcpy((uint8_t *)buf + first, data, n - first); }
    __sync_synchronize();
    r->head = head + n;
    return (int)n;
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

/* One active bridge: a socketpair end the pump owns + the slot's rings. */
typedef struct {
    volatile uint32_t *state;
    AsbRing *g2h; uint8_t *g2hData;   /* guest -> host (we read)  */
    AsbRing *h2g; uint8_t *h2gData;   /* host  -> guest (we write) */
    int      internal_fd;             /* our end of the socketpair; caller holds the other */
    volatile int stop;
    pthread_t thread;
} AsbPump;

@implementation AsbIvshmemTransport {
    int                 _fd;
    uint8_t            *_bar;
    uint64_t            _barSize;
    AsbShmDirectory    *_dir;
    AsbPump            *_pumps[MAX_PUMPS];
    int                 _pumpCount;
    pthread_mutex_t     _lock;
    /* Set under _lock by -close before it munmaps _bar. connectChannel re-checks it (and _bar) under
     * _lock before every BAR dereference, so a consumer's reconnect loop can't read a munmap'd BAR
     * (the VM-shutdown use-after-munmap crash on the agent queue). -close is idempotent. */
    volatile int        _closed;
}

- (nullable instancetype)initWithBackingPath:(NSString *)path
{
    if (!(self = [super init])) return nil;
    _fd = -1;
    pthread_mutex_init(&_lock, NULL);

    int fd = open(path.fileSystemRepresentation, O_RDWR);
    if (fd < 0) return nil;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return nil; }
    void *bar = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (bar == MAP_FAILED) { close(fd); return nil; }

    AsbShmDirectory *dir = (AsbShmDirectory *)bar;
    if (dir->magic != ASB_SHM_DIR_MAGIC) {   /* launcher hasn't published yet */
        munmap(bar, (size_t)st.st_size);
        close(fd);
        return nil;
    }
    _fd = fd;
    _bar = (uint8_t *)bar;
    _barSize = (uint64_t)st.st_size;
    _dir = dir;
    return self;
}

- (BOOL)mapped { return _bar != NULL; }

- (const AsbShmRegionDesc *)regionForChannel:(int)channel
{
    if (!_dir) return NULL;
    uint32_t n = _dir->n_regions < 16 ? _dir->n_regions : 16;
    for (uint32_t i = 0; i < n; i++)
        if (_dir->regions[i].channel_id == (uint32_t)channel) return &_dir->regions[i];
    return NULL;
}

- (int)connectChannel:(int)channel timeoutMs:(int)timeoutMs
{
    /* Claim a slot: a single-slot channel uses slot 0; a multi-slot channel (ssh) takes the first
     * FREE/CLOSED one. We arm it CONNECTING; the guest's asb_accept moves it to ESTABLISHED. */
    pthread_mutex_lock(&_lock);
    if (_closed || !_bar || !_dir) { pthread_mutex_unlock(&_lock); return -1; }
    if (_pumpCount >= MAX_PUMPS) { pthread_mutex_unlock(&_lock); return -1; }
    /* regionForChannel dereferences _dir (a pointer INTO the BAR); resolve it UNDER _lock with the
     * _closed/_bar guard. Otherwise -close (which munmaps the BAR under _lock when QEMU exits on a
     * guest shutdown) can free it between an unlocked check and the deref -> the EXC_BAD_ACCESS in
     * regionForChannel: that happens when the user shuts Windows down from inside the guest. */
    const AsbShmRegionDesc *r = [self regionForChannel:channel];
    if (!r) { pthread_mutex_unlock(&_lock); return -1; }
    uint8_t *slot = NULL;
    for (uint32_t i = 0; i < r->n_slots; i++) {
        uint8_t *s = _bar + r->offset + (uint64_t)i * r->slot_stride;
        volatile uint32_t *st = (volatile uint32_t *)s;
        if (*st == ASB_SLOT_FREE || *st == ASB_SLOT_CLOSED) { slot = s; break; }
    }
    if (!slot) { pthread_mutex_unlock(&_lock); return -1; }

    volatile uint32_t *state = (volatile uint32_t *)slot;
    AsbRing *g2h = (AsbRing *)(slot + ASB_SLOT_HDR);
    uint8_t *g2hData = (uint8_t *)g2h + ASB_RING_HDR;
    uint32_t g2hCap = g2h->cap;                            /* published by the launcher */
    AsbRing *h2g = (AsbRing *)(g2hData + g2hCap);
    uint8_t *h2gData = (uint8_t *)h2g + ASB_RING_HDR;

    g2h->head = g2h->tail = 0;
    h2g->head = h2g->tail = 0;
    __sync_synchronize();
    *state = ASB_SLOT_CONNECTING;
    pthread_mutex_unlock(&_lock);

    /* Wait for the guest to accept. Dereference *state ONLY under _lock with a _closed/_bar guard:
     * -close munmaps _bar under _lock, so this loop can never read a munmap'd BAR (the shutdown crash).
     * The lock is held only for the single read, not across the sleep, so -close is never blocked long. */
    int waited = 0;
    for (;;) {
        pthread_mutex_lock(&_lock);
        if (_closed || !_bar) { pthread_mutex_unlock(&_lock); return -1; }
        uint32_t cur = *state;
        pthread_mutex_unlock(&_lock);
        if (cur == ASB_SLOT_ESTABLISHED) break;
        if (waited >= timeoutMs) {
            pthread_mutex_lock(&_lock);
            if (!_closed && _bar) { __sync_synchronize(); *state = ASB_SLOT_FREE; }
            pthread_mutex_unlock(&_lock);
            return -1;
        }
        usleep(2000); waited += 2;
    }

    /* Release the slot we armed CONNECTING, but only if the BAR is still mapped (guarded like above). */
    #define ASB_RELEASE_SLOT() do { pthread_mutex_lock(&_lock); \
        if (!_closed && _bar) { __sync_synchronize(); *state = ASB_SLOT_FREE; } \
        pthread_mutex_unlock(&_lock); } while (0)

    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) { ASB_RELEASE_SLOT(); return -1; }
    /* The pump owns sp[1] and services BOTH directions on one thread; make its end non-blocking so a
     * backed-up guest->host write (relay/ssh client not draining yet) can never wedge the thread and
     * starve host->guest. The pump carries any unwritten bytes and retries on writability. The
     * caller's end (sp[0]) stays blocking — the dual-fd relay loop selects on it normally. */
    fcntl(sp[1], F_SETFL, fcntl(sp[1], F_GETFL, 0) | O_NONBLOCK);

    AsbPump *p = (AsbPump *)calloc(1, sizeof(AsbPump));
    if (!p) { close(sp[0]); close(sp[1]); ASB_RELEASE_SLOT(); return -1; }
    p->state = state; p->g2h = g2h; p->g2hData = g2hData; p->h2g = h2g; p->h2gData = h2gData;
    p->internal_fd = sp[1];

    /* Register the pump AND start its thread under one _lock with a final _closed recheck, so -close
     * (which sets _closed + munmaps under _lock) can't slip in between and leave a running pump that
     * dereferences a munmap'd BAR. */
    extern void *asb_ivshmem_pump_main(void *);
    pthread_mutex_lock(&_lock);
    if (_closed || !_bar || _pumpCount >= MAX_PUMPS) {
        pthread_mutex_unlock(&_lock);
        close(sp[0]); close(sp[1]); free(p);
        return -1;
    }
    if (pthread_create(&p->thread, NULL, asb_ivshmem_pump_main, p) != 0) {
        pthread_mutex_unlock(&_lock);
        close(sp[0]); close(sp[1]); free(p);
        return -1;
    }
    _pumps[_pumpCount++] = p;
    pthread_mutex_unlock(&_lock);
    #undef ASB_RELEASE_SLOT
    return sp[0];   /* caller's end */
}

- (void)close
{
    pthread_mutex_lock(&_lock);
    if (_closed) { pthread_mutex_unlock(&_lock); return; }   /* idempotent: called from QEMU exit AND core Stopped */
    _closed = 1;   /* connectChannel observes this under _lock and stops dereferencing the BAR */
    for (int i = 0; i < _pumpCount; i++) {
        AsbPump *p = _pumps[i];
        if (!p) continue;
        p->stop = 1;
        shutdown(p->internal_fd, SHUT_RDWR);   /* unblock the pump's select/recv */
    }
    int n = _pumpCount;
    pthread_mutex_unlock(&_lock);

    /* Join pumps OUTSIDE the lock (they take no lock, but may block briefly draining). After this no
     * pump thread touches the BAR, so the munmap below is safe w.r.t. pumps. */
    for (int i = 0; i < n; i++) {
        AsbPump *p = _pumps[i];
        if (p && p->thread) pthread_join(p->thread, NULL);
    }
    pthread_mutex_lock(&_lock);
    for (int i = 0; i < _pumpCount; i++) { free(_pumps[i]); _pumps[i] = NULL; }
    _pumpCount = 0;
    /* munmap UNDER _lock: connectChannel only reads the BAR while holding _lock with a _closed/_bar
     * guard, so this can never pull the mapping out from under a live dereference. */
    if (_bar) { munmap(_bar, (size_t)_barSize); _bar = NULL; _dir = NULL; }
    if (_fd >= 0) { close(_fd); _fd = -1; }
    pthread_mutex_unlock(&_lock);
}

- (void)dealloc { [self close]; pthread_mutex_destroy(&_lock); }

@end

/* Bridge one slot <-> the socketpair. Guest->host: drain g2h, write to the fd. Host->guest: read
 * the fd, write to h2g. Exits when the guest leaves ESTABLISHED, the caller closes its fd (EOF), or
 * -close signals stop. Releases the slot (FREE) so a later connectChannel can re-claim it. */
void *asb_ivshmem_pump_main(void *arg)
{
    AsbPump *p = (AsbPump *)arg;
    /* This single thread services BOTH directions of one ssh connection. It must never block on one
     * direction while the other has work, or a sustained full-duplex burst deadlocks: guest->host
     * blocked on a backed-up socketpair write would stop draining h2g, the guest's asb_send would
     * then spin forever on a full g2h, and the connection wedges (the "drops on larger output" bug).
     * So: internal_fd is non-blocking, we select() for read AND write readiness, and we carry any
     * partially-written guest->host chunk across iterations instead of blocking on it. */
    uint8_t g2hbuf[16384];   /* guest->host bytes drained from g2h, awaiting the socketpair write */
    int     g2hlen = 0, g2hoff = 0;   /* [g2hoff, g2hlen) still to be written to internal_fd */
    uint8_t h2gbuf[16384];   /* host->guest bytes read from internal_fd, awaiting the h2g ring */
    int     h2glen = 0, h2goff = 0;   /* [h2goff, h2glen) still to be written to h2g */

    while (!p->stop && *p->state == ASB_SLOT_ESTABLISHED) {
        int did_work = 0;

        /* host -> guest: push any pending bytes into the h2g ring (partial ok, never blocks). */
        while (h2goff < h2glen) {
            int w = ring_write_host(p->h2g, p->h2gData, h2gbuf + h2goff, h2glen - h2goff);
            if (w > 0) { h2goff += w; did_work = 1; } else break;   /* ring full: guest draining */
        }
        if (h2goff >= h2glen) h2goff = h2glen = 0;

        /* guest -> host: refill the staging buffer from g2h when it's empty. */
        if (g2hoff >= g2hlen) {
            g2hoff = g2hlen = 0;
            int n = ring_read_host(p->g2h, p->g2hData, g2hbuf, (int)sizeof(g2hbuf));
            if (n > 0) { g2hlen = n; did_work = 1; }
        }
        /* ...and flush as much of it to the socketpair as fits right now (non-blocking). */
        while (g2hoff < g2hlen) {
            ssize_t w = send(p->internal_fd, g2hbuf + g2hoff, (size_t)(g2hlen - g2hoff), 0);
            if (w > 0) { g2hoff += (int)w; did_work = 1; }
            else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;   /* fd full: select below */
            else goto done;   /* peer closed/error */
        }

        /* If both directions made progress this pass, loop again immediately; otherwise wait for the
         * one thing we're blocked on (fd readable for more h2g, fd writable to flush g2h) with a short
         * timeout so a ring-full stall still re-checks the rings promptly. */
        if (did_work) continue;

        fd_set rfds, wfds;
        FD_ZERO(&rfds); FD_ZERO(&wfds);
        FD_SET(p->internal_fd, &rfds);             /* want host->guest bytes (only if we can buffer) */
        int want_write = (g2hoff < g2hlen);        /* still have guest->host bytes to flush */
        if (want_write) FD_SET(p->internal_fd, &wfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 2000 };
        int s = select(p->internal_fd + 1, &rfds, want_write ? &wfds : NULL, NULL, &tv);
        if (s < 0) { if (errno == EINTR) continue; break; }
        if (s > 0 && FD_ISSET(p->internal_fd, &rfds) && h2glen == 0) {
            ssize_t rd = recv(p->internal_fd, h2gbuf, sizeof(h2gbuf), 0);
            if (rd == 0) break;   /* caller closed its end */
            if (rd < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
            if (rd > 0) h2glen = (int)rd;
        }
    }
done:
    close(p->internal_fd);
    p->internal_fd = -1;
    __sync_synchronize();
    *p->state = ASB_SLOT_FREE;   /* guest sees != ESTABLISHED -> disconnect; slot reusable */
    return NULL;
}
