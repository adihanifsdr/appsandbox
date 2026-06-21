/*
 * wim -- Windows Imaging (WIM/ESD) read-engine.
 *
 * The squashfs.c analog: parses a .wim/.esd (both are the WIM container; .esd
 * just uses LZMS solid compression), enumerates its resource/offset (lookup)
 * table, and streams decompressed file data out to the NTFS writer. Supports
 * all Win11 image variants: XPRESS + LZX (non-solid, install.wim) and LZMS
 * (solid, install.esd).
 *
 * A WIM is: a 208-byte header -> an offset/lookup table of 50-byte entries
 * (each a resource header + part + refcount + SHA-1 of the *uncompressed*
 * bytes) -> per-image METADATA resources (a security table + a DIRENTRY tree)
 * -> the file data resources. Resources are stored in 32768-byte chunks (or
 * LZMS solid blocks) with a chunk-offset/size table.
 */
#ifndef ASB_WIM_H
#define ASB_WIM_H

#include <stdint.h>
#include <stddef.h>

typedef enum { WIM_COMP_NONE = 0, WIM_COMP_XPRESS, WIM_COMP_LZX, WIM_COMP_LZMS } wim_comp_t;

/* Resource header (reshdr) flags. */
#define WIM_RESHDR_FREE     0x01
#define WIM_RESHDR_METADATA 0x02
#define WIM_RESHDR_COMPRESSED 0x04
#define WIM_RESHDR_SPANNED  0x08
#define WIM_RESHDR_SOLID    0x10

typedef struct {
    uint64_t size_in_wim;   /* stored (possibly compressed) size in the WIM file */
    uint64_t offset;        /* byte offset of the resource within the WIM */
    uint64_t orig_size;     /* uncompressed size */
    uint8_t  flags;         /* WIM_RESHDR_* */
    uint16_t part_number;
    uint32_t refcount;
    uint8_t  hash[20];      /* SHA-1 of the uncompressed resource (0 = none) */
} wim_resource_t;

typedef struct wim wim_t;

wim_t      *wim_open(const char *path);
void        wim_close(wim_t *w);

wim_comp_t  wim_compression(const wim_t *w);
uint32_t    wim_chunk_size(const wim_t *w);   /* 32768 typical; LZMS solid larger */
uint32_t    wim_image_count(const wim_t *w);
uint32_t    wim_boot_index(const wim_t *w);

size_t                 wim_num_resources(const wim_t *w);
const wim_resource_t  *wim_resource(const wim_t *w, size_t i);

/* The METADATA resource for a 1-based image index (NULL if out of range). */
const wim_resource_t  *wim_metadata_resource(const wim_t *w, uint32_t image_index);
/* Find a file-data resource by its uncompressed SHA-1 (NULL if absent). */
const wim_resource_t  *wim_lookup_by_hash(const wim_t *w, const uint8_t hash[20]);

/* The raw fd + a pread helper, used by the resource/chunk reader (resource.c). */
int wim_pread(const wim_t *w, void *buf, size_t len, uint64_t off);

/* Read a fully-UNCOMPRESSED resource (flags lacks WIM_RESHDR_COMPRESSED) into
 * `out` (must hold orig_size bytes). Returns 0/-1. Compressed resources go
 * through the chunk reader + decompressors (resource.c). */
int wim_read_uncompressed(const wim_t *w, const wim_resource_t *r, uint8_t *out);

/* Read a full resource (uncompressed, or XPRESS/LZX/LZMS chunked) into `out`
 * (must hold orig_size bytes). Dispatches per-chunk to the codecs. Returns 0 on
 * success, -1 on malformed input, -2 if solid (LZMS) and that path isn't wired
 * yet. Implemented in resource.c. */
int wim_read_resource(const wim_t *w, const wim_resource_t *r, uint8_t *out);

#endif /* ASB_WIM_H */
