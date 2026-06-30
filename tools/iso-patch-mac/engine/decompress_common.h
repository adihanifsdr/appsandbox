/*
 * decompress_common -- shared primitives for the WIM codecs (xpress.c, lzx.c):
 * a 16-bit-little-endian, MSB-first input bitstream and a single-level
 * canonical-Huffman decode table, as required by the LZX/XPRESS bitstream
 * format. See docs/appsandbox/codecs/lzx.md.
 */
#ifndef ASB_DECOMPRESS_COMMON_H
#define ASB_DECOMPRESS_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

/* ---- input bitstream: 16-bit LE coding units, bits consumed MSB-first ---- */
typedef struct {
    uint32_t       bitbuf;     /* left-justified: next bit to read is bit 31 */
    unsigned       bitsleft;
    const uint8_t *next;
    const uint8_t *end;
} bitstream_t;

static inline void bs_init(bitstream_t *bs, const uint8_t *p, size_t n) {
    bs->bitbuf = 0; bs->bitsleft = 0; bs->next = p; bs->end = p + n;
}
/* Ensure >= n bits are available (n <= 17). Missing input reads as zero. */
static inline void bs_ensure(bitstream_t *bs, unsigned n) {
    if (bs->bitsleft >= n) return;
    if ((size_t)(bs->end - bs->next) >= 2) {
        bs->bitbuf |= (uint32_t)(bs->next[0] | ((uint32_t)bs->next[1] << 8)) << (16 - bs->bitsleft);
        bs->next += 2; bs->bitsleft += 16;
        if (n == 17 && bs->bitsleft < 17) {
            if ((size_t)(bs->end - bs->next) >= 2) {
                bs->bitbuf |= (uint32_t)(bs->next[0] | ((uint32_t)bs->next[1] << 8)) << (16 - bs->bitsleft);
                bs->next += 2; bs->bitsleft += 16;
            } else { bs->bitsleft = 32; }
        }
    } else { bs->bitsleft = 32; }   /* exhausted: treat remaining as zero */
}
static inline uint32_t bs_peek(const bitstream_t *bs, unsigned n) {
    return n ? (bs->bitbuf >> (32 - n)) : 0;
}
static inline void bs_remove(bitstream_t *bs, unsigned n) {
    bs->bitbuf <<= n; bs->bitsleft -= n;
}
static inline uint32_t bs_read(bitstream_t *bs, unsigned n) {
    bs_ensure(bs, n); uint32_t v = bs_peek(bs, n); bs_remove(bs, n); return v;
}
static inline void bs_align(bitstream_t *bs) { bs->bitbuf = 0; bs->bitsleft = 0; }
static inline uint32_t bs_read_u32(bitstream_t *bs) {
    if ((size_t)(bs->end - bs->next) < 4) return 0;
    uint32_t v = bs->next[0] | ((uint32_t)bs->next[1] << 8) |
                 ((uint32_t)bs->next[2] << 16) | ((uint32_t)bs->next[3] << 24);
    bs->next += 4; return v;
}
static inline unsigned bs_bytes_left(const bitstream_t *bs) {
    return (unsigned)(bs->end - bs->next);
}

/* ---- canonical Huffman decode table ----
 * Single-level table of 2^max_len entries; each entry packs (symbol<<16)|len.
 * Codewords are assigned in canonical (length, symbol) order, MSB-first. */
typedef struct { uint32_t *table; unsigned max_len; } huff_t;

/* Build from per-symbol code lengths (0 = unused). Returns 0/-1. */
static inline int huff_build(huff_t *h, const uint8_t *lens, unsigned n, unsigned max_len) {
    h->max_len = max_len;
    size_t tsz = (size_t)1 << max_len;
    h->table = (uint32_t *)malloc(tsz * sizeof(uint32_t));
    if (!h->table) return -1;
    for (size_t i = 0; i < tsz; i++) h->table[i] = 0;     /* len 0 => invalid */

    unsigned counts[33] = { 0 };
    for (unsigned s = 0; s < n; s++) {
        if (lens[s] > max_len) { free(h->table); h->table = NULL; return -1; }
        counts[lens[s]]++;
    }
    counts[0] = 0;
    /* Reject over-subscribed code-length sets (Kraft inequality): a malformed set
     * whose codewords exceed the 2^max_len code space would overflow next_code[] and
     * write past the table (heap OOB). Incomplete (under-subscribed) sets are tolerated. */
    { int left = 1;
      for (unsigned l = 1; l <= max_len; l++) { left <<= 1; left -= (int)counts[l];
          if (left < 0) { free(h->table); h->table = NULL; return -1; } } }
    uint32_t code = 0, next_code[33], total = 0;
    for (unsigned l = 1; l <= max_len; l++) { next_code[l] = code; code = (code + counts[l]) << 1; total += counts[l]; }
    if (total == 0) return 0;                              /* empty code: decode sym 0, 0 bits */

    for (unsigned s = 0; s < n; s++) {
        unsigned l = lens[s];
        if (!l) continue;
        uint32_t c    = next_code[l]++;
        uint32_t base = c << (max_len - l);
        uint32_t cnt  = 1u << (max_len - l);
        uint32_t entry = ((uint32_t)s << 16) | l;
        for (uint32_t k = 0; k < cnt; k++) h->table[base + k] = entry;
    }
    return 0;
}
static inline void huff_free(huff_t *h) { free(h->table); h->table = NULL; }

static inline unsigned huff_decode(bitstream_t *bs, const huff_t *h) {
    bs_ensure(bs, h->max_len);
    uint32_t e = h->table[bs_peek(bs, h->max_len)];
    bs_remove(bs, e & 0xffff);
    return e >> 16;
}

/* Overlap-safe LZ copy (offset may be < len). */
static inline void lz_copy(uint8_t *out, uint32_t off, uint32_t len) {
    const uint8_t *src = out - off;
    for (uint32_t i = 0; i < len; i++) out[i] = src[i];
}

#endif /* ASB_DECOMPRESS_COMMON_H */
