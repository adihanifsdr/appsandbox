#ifndef ASB_XPRESS_H
#define ASB_XPRESS_H
#include <stddef.h>
/* Decompress one XPRESS chunk. 0 on success, -1 on error. */
int xpress_decompress(const void *in, size_t in_size, void *out, size_t out_size);
#endif
