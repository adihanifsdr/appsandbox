/*
 * test_resource.c -- self-contained validation gate for the WIM chunk reader +
 * LZX decoder. For each compressed, non-solid resource: decompress via
 * wim_read_resource, SHA-1 the result, and require it to equal the lookup-table
 * hash. No external tools. Exits non-zero on any mismatch/decode failure.
 */
#include "wim.h"
#include "sha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1]
        : "/Volumes/CCCOMA_A64FRE_EN-US_DV9/sources/install.wim";
    size_t limit = argc > 2 ? strtoul(argv[2], NULL, 10) : 4000;

    wim_t *w = wim_open(path);
    if (!w) { fprintf(stderr, "wim_open failed: %s\n", path); return 2; }
    uint32_t cs = wim_chunk_size(w);
    size_t n = wim_num_resources(w);
    printf("comp=%d chunk=%u images=%u resources=%zu\n",
           wim_compression(w), cs, wim_image_count(w), n);

    size_t tested=0, ok=0, fail=0, sk_solid=0, sk_big=0, single=0, multi=0;
    uint64_t maxok = 0;
    unsigned char hash[20];

    for (size_t i = 0; i < n && tested < limit; i++) {
        const wim_resource_t *r = wim_resource(w, i);
        if (!(r->flags & WIM_RESHDR_COMPRESSED)) continue;
        if (r->flags & WIM_RESHDR_SOLID) { sk_solid++; continue; }
        if (r->orig_size == 0) continue;
        int hz = 1; for (int k = 0; k < 20; k++) if (r->hash[k]) { hz = 0; break; }
        if (hz) continue;
        if (r->orig_size > 64ull*1024*1024) { sk_big++; continue; }

        uint8_t *buf = malloc((size_t)r->orig_size);
        if (!buf) { fprintf(stderr, "OOM %llu\n", (unsigned long long)r->orig_size); break; }
        int rc = wim_read_resource(w, r, buf);
        tested++;
        if (rc != 0) {
            fail++;
            if (fail <= 12) printf("  [%zu] DECODE rc=%d orig=%llu csize=%llu\n",
                i, rc, (unsigned long long)r->orig_size, (unsigned long long)r->size_in_wim);
            free(buf); continue;
        }
        sha1(buf, (size_t)r->orig_size, hash);
        if (memcmp(hash, r->hash, 20) == 0) {
            ok++;
            if (r->orig_size > cs) multi++; else single++;
            if (r->orig_size > maxok) maxok = r->orig_size;
        } else {
            fail++;
            if (fail <= 12) printf("  [%zu] HASH MISMATCH orig=%llu csize=%llu chunks=%llu\n",
                i, (unsigned long long)r->orig_size, (unsigned long long)r->size_in_wim,
                (unsigned long long)((r->orig_size + cs - 1)/cs));
        }
        free(buf);
    }
    printf("tested=%zu OK=%zu FAIL=%zu  (single-chunk=%zu multi-chunk=%zu maxOK=%llu B)  skip_solid=%zu skip_big=%zu\n",
           tested, ok, fail, single, multi, (unsigned long long)maxok, sk_solid, sk_big);
    wim_close(w);
    return fail ? 1 : 0;
}
