/* blockio.c -- see blockio.h. */

#include "blockio.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#define COAL_CAP (16ull * 1024 * 1024)   /* 16 MiB, matches ext4.c */

struct blockio {
    int       fd;
    uint64_t  size;

    uint8_t  *coal;        /* coalescing buffer */
    uint64_t  coal_off;    /* absolute offset of coal[0] */
    size_t    coal_len;    /* bytes currently buffered */
    int       coal_active;

    /* stats */
    uint64_t  bytes_in;
    uint64_t  bytes_flushed;
    uint64_t  flush_calls;
};

/* pwrite the whole buffer, looping on partial writes / EINTR. */
static int pwrite_full(int fd, const void *buf, size_t len, uint64_t off) {
    const uint8_t *p = buf; size_t done = 0;
    while (done < len) {
        ssize_t w = pwrite(fd, p + done, len - done, (off_t)(off + done));
        if (w > 0) { done += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}
static int pread_full(int fd, void *buf, size_t len, uint64_t off) {
    uint8_t *p = buf; size_t done = 0;
    while (done < len) {
        ssize_t r = pread(fd, p + done, len - done, (off_t)(off + done));
        if (r > 0) { done += (size_t)r; continue; }
        if (r == 0) { memset(p + done, 0, len - done); return 0; }   /* hole / past EOF */
        if (errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static int io_flush(blockio_t *io) {
    if (!io->coal_active || io->coal_len == 0) { io->coal_active = 0; io->coal_len = 0; return 0; }
    int rc = pwrite_full(io->fd, io->coal, io->coal_len, io->coal_off);
    io->bytes_flushed += io->coal_len;
    io->flush_calls++;
    io->coal_active = 0;
    io->coal_len = 0;
    return rc;
}

static blockio_t *io_alloc(int fd, uint64_t size) {
    blockio_t *io = calloc(1, sizeof *io);
    if (!io) return NULL;
    io->coal = malloc(COAL_CAP);
    if (!io->coal) { free(io); return NULL; }
    io->fd = fd;
    io->size = size;
    return io;
}

blockio_t *blockio_create(const char *path, uint64_t size_bytes) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return NULL;
    if (ftruncate(fd, (off_t)size_bytes) != 0) { close(fd); return NULL; }
    blockio_t *io = io_alloc(fd, size_bytes);
    if (!io) { close(fd); return NULL; }
    return io;
}

blockio_t *blockio_open(const char *path) {
    int fd = open(path, O_RDWR);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return NULL; }
    blockio_t *io = io_alloc(fd, (uint64_t)st.st_size);
    if (!io) { close(fd); return NULL; }
    return io;
}

int blockio_write(blockio_t *io, uint64_t off, const void *buf, size_t len) {
    io->bytes_in += len;
    const uint8_t *src = buf;
    while (len > 0) {
        if (io->coal_active && off == io->coal_off + io->coal_len) {
            size_t room = COAL_CAP - io->coal_len;
            size_t chunk = len < room ? len : room;
            memcpy(io->coal + io->coal_len, src, chunk);
            io->coal_len += chunk; src += chunk; off += chunk; len -= chunk;
            if (io->coal_len == COAL_CAP && io_flush(io) != 0) return -1;
            continue;
        }
        if (io_flush(io) != 0) return -1;
        io->coal_off = off;
        io->coal_active = 1;
    }
    return 0;
}

int blockio_pwrite(blockio_t *io, uint64_t off, const void *buf, size_t len) {
    /* Direct, lock-free pwrite to the fd. pwrite() does not use the fd's seek
     * pointer, so concurrent calls to disjoint ranges are safe. Deliberately
     * does NOT touch the coalescing buffer or any blockio_t mutable field
     * (only reads io->fd), so it needs no synchronization with other
     * blockio_pwrite callers. bytes_in/flushed stats are intentionally skipped
     * to avoid a data race; the parallel extractor tracks its own byte total. */
    return pwrite_full(io->fd, buf, len, off);
}

int blockio_zero(blockio_t *io, uint64_t off, uint64_t n) {
    static const uint8_t zeros[65536] = { 0 };
    while (n > 0) {
        size_t chunk = n > sizeof zeros ? sizeof zeros : (size_t)n;
        if (blockio_write(io, off, zeros, chunk) != 0) return -1;
        off += chunk; n -= chunk;
    }
    return 0;
}

int blockio_read(blockio_t *io, uint64_t off, void *buf, size_t len) {
    /* Simplest correctness: flush pending writes so the file reflects them,
     * then read. (Authoring is write-mostly; reads are rare.) */
    if (io_flush(io) != 0) return -1;
    return pread_full(io->fd, buf, len, off);
}

int blockio_flush(blockio_t *io) { return io_flush(io); }
uint64_t blockio_size(const blockio_t *io) { return io->size; }

int blockio_close(blockio_t *io) {
    if (!io) return 0;
    int rc = io_flush(io);
    if (io->fd >= 0) { if (fsync(io->fd) != 0) rc = -1; close(io->fd); }
    free(io->coal);
    free(io);
    return rc;
}
