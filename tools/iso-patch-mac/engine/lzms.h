#ifndef ASB_LZMS_H
#define ASB_LZMS_H
#include <stddef.h>
/* Decompress one LZMS (solid/ESD) chunk. 0 on success, -1 on error. */
int lzms_decompress(const void *in, size_t in_size, void *out, size_t out_size);
#endif
