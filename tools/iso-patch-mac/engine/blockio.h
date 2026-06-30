/*
 * blockio -- raw disk-image block I/O for the macOS Windows-disk builder.
 *
 * The portable analogue of the inline I/O layer in tools/iso-patch/engine/ext4.c
 * (which writes to a Windows \\.\PhysicalDrive HANDLE). Here the target is a raw
 * image FILE on macOS: we compute every byte offset ourselves and pwrite() into
 * the file, so authoring NTFS/FAT32/GPT needs no hdiutil/diskutil mount. APFS
 * sparse files give dynamic-disk behaviour (holes cost nothing).
 *
 * Like ext4.c, all writes route through a 16 MiB coalescing buffer: contiguous
 * adjacent writes append to the active batch and flush as one large pwrite,
 * turning hundreds of thousands of tiny writes into a few big ones.
 *
 * Offsets are ABSOLUTE image byte offsets. Filesystem writers (ntfs/fat32) are
 * handed their partition's base offset and add it themselves.
 */
#ifndef ASB_BLOCKIO_H
#define ASB_BLOCKIO_H

#include <stdint.h>
#include <stddef.h>

typedef struct blockio blockio_t;

/* Create (or truncate) a raw image file of exactly `size_bytes` and open it
 * read/write. Returns NULL on failure. */
blockio_t *blockio_create(const char *path, uint64_t size_bytes);

/* Open an existing image read/write without truncating. */
blockio_t *blockio_open(const char *path);

/* Coalesced write at absolute offset. Returns 0 on success, -1 on error. */
int blockio_write(blockio_t *io, uint64_t off, const void *buf, size_t len);

/* Direct pwrite at absolute offset, bypassing the coalescing buffer. Goes
 * straight to the fd, so it is SAFE to call concurrently from multiple threads
 * AS LONG AS the [off,off+len) ranges are disjoint across threads and do not
 * overlap any region currently buffered in the coalescer. Used by the parallel
 * file-data extractor (each file's clusters are disjoint by allocation). The
 * coalescing path must be idle (no pending main-thread metadata in this range)
 * while these run. Returns 0/-1. */
int blockio_pwrite(blockio_t *io, uint64_t off, const void *buf, size_t len);

/* Read at absolute offset. Flushes any pending coalesced writes that the read
 * range could observe, then preads. Returns 0/-1. */
int blockio_read(blockio_t *io, uint64_t off, void *buf, size_t len);

/* Write `n` zero bytes at `off` (chunked through the coalescer). */
int blockio_zero(blockio_t *io, uint64_t off, uint64_t n);

/* Flush the coalescing buffer to the file. */
int blockio_flush(blockio_t *io);

/* Total image size in bytes. */
uint64_t blockio_size(const blockio_t *io);

/* Flush, close the fd, free. Returns 0/-1 (flush failure). */
int blockio_close(blockio_t *io);

#endif /* ASB_BLOCKIO_H */
