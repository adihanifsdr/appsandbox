/*
 * lzx -- LZX chunk decompressor for WIM (window 2^15). See lzx.c.
 */
#ifndef ASB_LZX_H
#define ASB_LZX_H

#include <stddef.h>

/* Decompress one LZX chunk. `out_size` is the exact uncompressed chunk size
 * (<= 32768). Returns 0 on success, -1 on malformed input. */
int lzx_decompress(const void *in, size_t in_size, void *out, size_t out_size);

#endif /* ASB_LZX_H */
