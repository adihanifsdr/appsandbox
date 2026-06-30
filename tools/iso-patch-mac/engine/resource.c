/*
 * resource.c -- read a full WIM resource into memory, decompressing per-chunk.
 * Handles uncompressed and non-solid (XPRESS/LZX) resources; solid (LZMS) is
 * dispatched once lzms.c lands. Chunk-table arithmetic per the verified spec
 * (docs/appsandbox/codecs/WIM.md): cumulative offsets, num_chunks-1 entries,
 * 4-byte if resource < 4 GiB else 8-byte, stored-chunk (csize==usize) memcpy.
 */
#include "wim.h"
#include "lzx.h"
#include "xpress.h"
#include "lzms.h"
#include <stdlib.h>
#include <string.h>

static uint64_t entry_at(const uint8_t *tbl, unsigned esz, uint64_t idx) {
    const uint8_t *p = tbl + idx * esz;
    if (esz == 4) return (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24));
    uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8*i); return v;
}

int wim_read_resource(const wim_t *w, const wim_resource_t *r, uint8_t *out) {
    if (r->flags & WIM_RESHDR_SOLID) return -2;           /* solid/LZMS via lzms path (TODO wiring) */
    if (!(r->flags & WIM_RESHDR_COMPRESSED))              /* stored uncompressed */
        return wim_pread(w, out, (size_t)r->orig_size, r->offset);

    wim_comp_t comp = wim_compression(w);
    uint32_t chunk_size = wim_chunk_size(w);
    uint64_t orig = r->orig_size;
    if (orig == 0) return 0;
    uint64_t num_chunks = (orig + chunk_size - 1) / chunk_size;
    unsigned esz = (orig <= 0xFFFFFFFFull) ? 4 : 8;
    uint64_t num_entries = num_chunks - 1;
    uint64_t table_size = num_entries * esz;

    uint8_t *table = NULL;
    if (table_size) {
        table = malloc((size_t)table_size);
        if (!table) return -1;
        if (wim_pread(w, table, (size_t)table_size, r->offset) != 0) { free(table); return -1; }
    }
    uint64_t data_start = r->offset + table_size;
    uint64_t data_size  = r->size_in_wim - table_size;

    uint8_t *cbuf = malloc(chunk_size);                   /* csize <= usize <= chunk_size */
    if (!cbuf) { free(table); return -1; }

    int rc = 0;
    for (uint64_t i = 0; i < num_chunks; i++) {
        uint64_t cstart = (i == 0) ? 0 : entry_at(table, esz, i - 1);
        uint64_t cend   = (i == num_chunks - 1) ? data_size : entry_at(table, esz, i);
        if (cend < cstart || cend > data_size) { rc = -1; break; }
        size_t csize = (size_t)(cend - cstart);
        size_t usize = (i == num_chunks - 1) ? (size_t)(orig - (num_chunks - 1) * (uint64_t)chunk_size)
                                             : chunk_size;
        if (csize == 0 || csize > chunk_size) { rc = -1; break; }
        if (wim_pread(w, cbuf, csize, data_start + cstart) != 0) { rc = -1; break; }
        uint8_t *dst = out + i * (uint64_t)chunk_size;
        if (csize == usize) {                             /* stored chunk */
            memcpy(dst, cbuf, usize);
        } else {
            int d;
            switch (comp) {
                case WIM_COMP_LZX:    d = lzx_decompress(cbuf, csize, dst, usize); break;
                case WIM_COMP_XPRESS: d = xpress_decompress(cbuf, csize, dst, usize); break;
                case WIM_COMP_LZMS:   d = lzms_decompress(cbuf, csize, dst, usize); break;
                default:              d = -1; break;
            }
            if (d != 0) { rc = -1; break; }
        }
    }
    free(table);
    free(cbuf);
    return rc;
}
