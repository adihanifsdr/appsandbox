/* asb_t_host.c — macOS host harness for the asb_transport ivshmem channel test.
 * Maps the ivshmem backing file, lays out the directory + a ch1 (agent) stream region, then acts as
 * the host connector: sets the slot CONNECTING, waits ESTABLISHED, recv "hello", send "1:ping",
 * recv "1:pong". Proves the partitioned ring transport end-to-end against asb_t_guest.exe.
 * Build: cc -O2 -o /tmp/asb_t_host asb_t_host.c    Run: /tmp/asb_t_host /tmp/ivshmem.bin */
#include "../asb_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define RING_HDR ((uint32_t)sizeof(AsbRing))

/* host is consumer of g2h, producer of h2g */
static int hring_read(AsbRing *r, uint8_t *data, void *buf, int len) {
    uint64_t head, tail; uint32_t cap, used, n, first;
    __sync_synchronize();
    head = r->head; tail = r->tail; cap = r->cap;
    used = (uint32_t)(tail - head); n = (uint32_t)len < used ? (uint32_t)len : used;
    if (!n) return 0;
    first = cap - (uint32_t)(head & (cap - 1));
    if (first >= n) memcpy(buf, data + (head & (cap - 1)), n);
    else { memcpy(buf, data + (head & (cap - 1)), first); memcpy((uint8_t*)buf + first, data, n - first); }
    __sync_synchronize(); r->head = head + n; return (int)n;
}
static int hring_write(AsbRing *r, uint8_t *data, const void *buf, int len) {
    uint64_t head, tail; uint32_t cap, used, freeb, n, first;
    __sync_synchronize();
    head = r->head; tail = r->tail; cap = r->cap;
    used = (uint32_t)(tail - head); freeb = cap - used; n = (uint32_t)len < freeb ? (uint32_t)len : freeb;
    if (!n) return 0;
    first = cap - (uint32_t)(tail & (cap - 1));
    if (first >= n) memcpy(data + (tail & (cap - 1)), buf, n);
    else { memcpy(data + (tail & (cap - 1)), buf, first); memcpy(data, (const uint8_t*)buf + first, n - first); }
    __sync_synchronize(); r->tail = tail + n; return (int)n;
}
static int host_recv_line(AsbRing *g2h, uint8_t *g2h_data, volatile uint32_t *state, char *buf, int n) {
    int pos = 0;
    while (pos < n - 1) {
        char ch; int r = hring_read(g2h, g2h_data, &ch, 1);
        if (r == 0) { __sync_synchronize(); if (*state >= ASB_SLOT_CLOSING) return -1; usleep(200); continue; }
        if (ch == '\n') break; if (ch != '\r') buf[pos++] = ch;
    }
    buf[pos] = 0; return pos;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/tmp/ivshmem.bin";
    int fd = open(path, O_RDWR); struct stat st;
    uint8_t *bar; AsbShmDirectory *dir;
    uint64_t region_off = 0x1000, region_size = 256 * 1024; uint32_t cap = 65536, slot_stride;
    uint8_t *slot; volatile uint32_t *state; AsbRing *g2h, *h2g; uint8_t *g2h_data, *h2g_data;
    char line[256]; int i;

    if (fd < 0) { perror("open"); return 1; }
    fstat(fd, &st);
    bar = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (bar == MAP_FAILED) { perror("mmap"); return 1; }
    dir = (AsbShmDirectory *)bar;

    slot_stride = ASB_SLOT_HDR + RING_HDR + cap + RING_HDR + cap;
    slot = bar + region_off;
    memset(slot, 0, slot_stride);
    state = (volatile uint32_t *)slot;
    g2h = (AsbRing *)(slot + ASB_SLOT_HDR); g2h->head = g2h->tail = 0; g2h->cap = cap;
    g2h_data = (uint8_t *)g2h + RING_HDR;
    h2g = (AsbRing *)(g2h_data + cap); h2g->head = h2g->tail = 0; h2g->cap = cap;
    h2g_data = (uint8_t *)h2g + RING_HDR;
    *state = ASB_SLOT_FREE;

    memset(bar, 0, 4096);  /* directory page; re-zero (slot is at 0x1000, untouched) */
    /* NB: we zeroed the directory page AFTER the slot init (slot is past 0x1000), so re-init dir fields: */
    dir->version = ASB_SHM_VERSION; dir->bar_size = (uint32_t)st.st_size; dir->n_regions = 1;
    dir->regions[0].channel_id = ASB_CH_AGENT; dir->regions[0].flags = ASB_REGION_STREAM;
    dir->regions[0].offset = region_off; dir->regions[0].size = region_size;
    dir->regions[0].n_slots = 1; dir->regions[0].slot_stride = slot_stride;
    __sync_synchronize();
    dir->magic = ASB_SHM_DIR_MAGIC;   /* publish last so the guest sees a complete directory */
    __sync_synchronize();
    printf("[host] directory published: ch1 region @0x%llx size=%lluK cap=%u stride=%u\n",
           (unsigned long long)region_off, (unsigned long long)region_size/1024, cap, slot_stride);

    printf("[host] connecting (slot CONNECTING), waiting for guest accept...\n"); fflush(stdout);
    *state = ASB_SLOT_CONNECTING; __sync_synchronize();
    for (i = 0; ; i++) { __sync_synchronize(); if (*state == ASB_SLOT_ESTABLISHED) break;
        if (i > 30000) { printf("[host] accept TIMEOUT (guest not listening?)\n"); return 1; } usleep(1000); }
    printf("[host] ESTABLISHED\n"); fflush(stdout);

    if (host_recv_line(g2h, g2h_data, state, line, sizeof(line)) < 0) { printf("[host] closed before hello\n"); return 1; }
    printf("[host] recv: '%s'  (expect 'hello')\n", line); fflush(stdout);

    hring_write(h2g, h2g_data, "1:ping\n", 7); __sync_synchronize();
    printf("[host] sent: '1:ping'\n"); fflush(stdout);

    if (host_recv_line(g2h, g2h_data, state, line, sizeof(line)) < 0) { printf("[host] closed before reply\n"); return 1; }
    printf("[host] recv: '%s'  (expect '1:pong')\n", line);
    printf("[host] %s\n", strcmp(line, "1:pong") == 0 ? "PASS — bidirectional ring transport works" : "MISMATCH");
    return 0;
}
