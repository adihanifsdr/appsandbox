/*
 * lzx.c -- LZX chunk decompressor for WIM (window 2^15, E8 always-on).
 * Implements the LZX (WIM variant) bitstream format per the on-disk spec in
 * docs/appsandbox/codecs/lzx.md. Decodes one 32768-byte chunk.
 */
#include "lzx.h"
#include "decompress_common.h"
#include <string.h>

#define LZX_NUM_MAIN_SYMS  496      /* window 15: 256 + 30*8 */
#define LZX_NUM_LEN_SYMS   249
#define LZX_NUM_ALIGNED    8
#define LZX_MIN_ALIGNED_SLOT 8
#define LZX_WIM_E8_FILESIZE 12000000

/* Offset-slot bases (already +2 adjusted; slots 0..2 are fake/unused-by-explicit). */
static const int32_t lzx_offset_slot_base[51] = {
    -2,-1,0,1,2, 4,6,10,14,22, 30,46,62,94,126, 190,254,382,510,766,
    1022,1534,2046,3070,4094, 6142,8190,12286,16382,24574, 32766,49150,65534,98302,131070,
    196606,262142,393214,524286,655358, 786430,917502,1048574,1179646,1310718,
    1441790,1572862,1703934,1835006,1966078, 2097150
};
static const uint8_t lzx_extra_offset_bits[50] = {
    0,0,0,0,1, 1,2,2,3,3, 4,4,5,5,6, 6,7,7,8,8,
    9,9,10,10,11, 11,12,12,13,13, 14,14,15,15,16, 16,17,17,17,17,
    17,17,17,17,17, 17,17,17,17,17
};

/* Read `num_lens` code lengths via the 20-symbol precode (delta-RLE, in place
 * over the PREVIOUS block's lengths). See spec "huffman". */
static int lzx_read_lens(bitstream_t *bs, uint8_t *lens, unsigned num_lens) {
    uint8_t pre[20];
    for (int i = 0; i < 20; i++) pre[i] = (uint8_t)bs_read(bs, 4);
    huff_t pc;
    if (huff_build(&pc, pre, 20, 15) != 0) return -1;
    unsigned i = 0;
    while (i < num_lens) {
        unsigned ps = huff_decode(bs, &pc);
        if (ps < 17) {
            int v = (int)lens[i] - (int)ps; if (v < 0) v += 17;
            lens[i++] = (uint8_t)v;
        } else if (ps == 17) {
            unsigned run = 4 + bs_read(bs, 4);
            while (run-- && i < num_lens) lens[i++] = 0;
        } else if (ps == 18) {
            unsigned run = 20 + bs_read(bs, 5);
            while (run-- && i < num_lens) lens[i++] = 0;
        } else { /* 19: run of a delta-coded length */
            unsigned run = 4 + bs_read(bs, 1);
            unsigned ps2 = huff_decode(bs, &pc);
            if (ps2 > 17) { huff_free(&pc); return -1; }
            int v = (int)lens[i] - (int)ps2; if (v < 0) v += 17;
            while (run-- && i < num_lens) lens[i++] = (uint8_t)v;
        }
    }
    huff_free(&pc);
    return 0;
}

static void lzx_e8_postprocess(uint8_t *data, size_t size) {
    if (size <= 10) return;
    size_t i = 0, end = size - 10;
    do {
        if (data[i] == 0xE8) {
            int32_t abs_val = (int32_t)((uint32_t)data[i+1] | ((uint32_t)data[i+2] << 8) |
                                        ((uint32_t)data[i+3] << 16) | ((uint32_t)data[i+4] << 24));
            int32_t pos = (int32_t)i, rel;
            int do_it = 0;
            if (abs_val >= 0 && abs_val < LZX_WIM_E8_FILESIZE) { rel = abs_val - pos; do_it = 1; }
            else if (abs_val < 0 && abs_val >= -pos)           { rel = abs_val + LZX_WIM_E8_FILESIZE; do_it = 1; }
            if (do_it) {
                data[i+1] = (uint8_t)rel;       data[i+2] = (uint8_t)(rel >> 8);
                data[i+3] = (uint8_t)(rel >> 16); data[i+4] = (uint8_t)(rel >> 24);
            }
            i += 5;
        } else i++;
    } while (i < end);
}

int lzx_decompress(const void *cin, size_t in_size, void *cout, size_t out_size) {
    bitstream_t bs;
    bs_init(&bs, cin, in_size);
    uint8_t *out = cout;
    size_t pos = 0;

    uint32_t recent[3] = { 1, 1, 1 };
    uint8_t  maincode_lens[LZX_NUM_MAIN_SYMS];
    uint8_t  lencode_lens[LZX_NUM_LEN_SYMS];
    uint8_t  aligned_lens[LZX_NUM_ALIGNED];
    memset(maincode_lens, 0, sizeof maincode_lens);
    memset(lencode_lens, 0, sizeof lencode_lens);
    int may_have_e8 = 0;

    while (pos < out_size) {
        bs_ensure(&bs, 4);
        unsigned btype = bs_read(&bs, 3);
        unsigned bsize = bs_read(&bs, 1) ? 32768u : bs_read(&bs, 16);   /* window 15: 16-bit size */
        if (bsize == 0 || pos + bsize > out_size) return -1;
        size_t block_end = pos + bsize;

        if (btype == 3) {                                  /* UNCOMPRESSED */
            bs_ensure(&bs, 1);
            bs_align(&bs);
            recent[0] = bs_read_u32(&bs); recent[1] = bs_read_u32(&bs); recent[2] = bs_read_u32(&bs);
            if (recent[0] == 0 || recent[1] == 0 || recent[2] == 0) return -1;
            if (bs_bytes_left(&bs) < bsize) return -1;
            memcpy(out + pos, bs.next, bsize);
            bs.next += bsize;
            if ((bsize & 1) && bs.next < bs.end) bs.next++;   /* pad byte */
            pos = block_end;
            may_have_e8 = 1;
            continue;
        }
        if (btype != 1 && btype != 2) return -1;

        if (btype == 2)
            for (int i = 0; i < 8; i++) aligned_lens[i] = (uint8_t)bs_read(&bs, 3);
        if (lzx_read_lens(&bs, maincode_lens, 256) != 0) return -1;
        if (lzx_read_lens(&bs, maincode_lens + 256, LZX_NUM_MAIN_SYMS - 256) != 0) return -1;
        if (lzx_read_lens(&bs, lencode_lens, LZX_NUM_LEN_SYMS) != 0) return -1;
        if (maincode_lens[0xE8]) may_have_e8 = 1;

        huff_t mainc, lenc, alignedc;
        if (huff_build(&mainc, maincode_lens, LZX_NUM_MAIN_SYMS, 16) != 0) return -1;
        if (huff_build(&lenc, lencode_lens, LZX_NUM_LEN_SYMS, 16) != 0) { huff_free(&mainc); return -1; }
        int is_aligned = (btype == 2);
        if (is_aligned && huff_build(&alignedc, aligned_lens, LZX_NUM_ALIGNED, 7) != 0) {
            huff_free(&mainc); huff_free(&lenc); return -1;
        }

        int rc = 0;
        while (pos < block_end) {
            unsigned sym = huff_decode(&bs, &mainc);
            if (sym < 256) { out[pos++] = (uint8_t)sym; continue; }
            unsigned length = (sym - 256) & 7;
            unsigned slot   = (sym - 256) >> 3;
            if (length == 7) length += huff_decode(&bs, &lenc);
            length += 2;
            uint32_t offset;
            if (slot < 3) {
                offset = recent[slot];
                recent[slot] = recent[0];
            } else {
                unsigned extra = lzx_extra_offset_bits[slot];
                if (is_aligned && slot >= LZX_MIN_ALIGNED_SLOT) {
                    offset = (uint32_t)bs_read(&bs, extra - 3) << 3;
                    offset |= huff_decode(&bs, &alignedc);
                } else {
                    offset = bs_read(&bs, extra);
                }
                offset = (uint32_t)((int32_t)offset + lzx_offset_slot_base[slot]);
                recent[2] = recent[1]; recent[1] = recent[0];
            }
            recent[0] = offset;
            if (offset == 0 || offset > pos || pos + length > out_size) { rc = -1; break; }
            lz_copy(out + pos, offset, length);
            pos += length;
        }
        huff_free(&mainc); huff_free(&lenc);
        if (is_aligned) huff_free(&alignedc);
        if (rc != 0) return -1;
    }

    if (may_have_e8) lzx_e8_postprocess(out, out_size);
    return 0;
}
