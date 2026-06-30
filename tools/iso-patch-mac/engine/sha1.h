/*
 * sha1 -- minimal pure-C SHA-1 (no OpenSSL/CommonCrypto dependency).
 * Used to verify WIM resource integrity: every lookup-table entry stores the
 * SHA-1 of the *uncompressed* resource, so decompressor output is validated by
 * hashing it and comparing — a self-contained correctness gate, no external tool.
 */
#ifndef ASB_SHA1_H
#define ASB_SHA1_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t h[5];
    uint64_t total;        /* total bytes hashed */
    uint8_t  buf[64];
    size_t   buflen;
} sha1_ctx;

void sha1_init(sha1_ctx *c);
void sha1_update(sha1_ctx *c, const void *data, size_t len);
void sha1_final(sha1_ctx *c, uint8_t out[20]);

/* One-shot helper. */
void sha1(const void *data, size_t len, uint8_t out[20]);

#endif /* ASB_SHA1_H */
