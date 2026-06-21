/* wim.c -- WIM/ESD header + offset(lookup)-table parser. See wim.h. */

#include "wim.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* WIM header flag bits (dwFlags @ 0x10). */
#define WIM_FLAG_COMPRESSION   0x00000002
#define WIM_FLAG_COMP_XPRESS   0x00020000
#define WIM_FLAG_COMP_LZX      0x00040000
#define WIM_FLAG_COMP_LZMS     0x00080000

#define LOOKUP_ENTRY_SIZE 50

struct wim {
    int             fd;
    wim_comp_t      comp;
    uint32_t        chunk_size;
    uint32_t        image_count;
    uint32_t        boot_index;
    wim_resource_t  offset_table_res;   /* reshdr of the lookup table itself */
    wim_resource_t *res;                /* parsed lookup entries */
    size_t          nres;
};

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24)); }
static uint64_t le64(const uint8_t *p) { uint64_t v=0; for(int i=0;i<8;i++) v|=(uint64_t)p[i]<<(8*i); return v; }

int wim_pread(const wim_t *w, void *buf, size_t len, uint64_t off) {
    uint8_t *p = buf; size_t done = 0;
    while (done < len) {
        ssize_t r = pread(w->fd, p + done, len - done, (off_t)(off + done));
        if (r > 0) { done += (size_t)r; continue; }
        if (r == 0) return -1;                 /* short read */
        if (errno == EINTR) continue;
        return -1;
    }
    return 0;
}

/* reshdr: 7-byte size + 1 flags byte, then u64 offset, then u64 original size. */
static void parse_reshdr(const uint8_t *p, wim_resource_t *r) {
    uint64_t sz = 0; for (int i = 0; i < 7; i++) sz |= (uint64_t)p[i] << (8*i);
    r->size_in_wim = sz;
    r->flags       = p[7];
    r->offset      = le64(p + 8);
    r->orig_size   = le64(p + 16);
}

wim_t *wim_open(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    wim_t *w = calloc(1, sizeof *w);
    if (!w) { close(fd); return NULL; }
    w->fd = fd;

    uint8_t hdr[208];
    if (wim_pread(w, hdr, sizeof hdr, 0) != 0) goto fail;
    if (memcmp(hdr, "MSWIM\0\0\0", 8) != 0) goto fail;          /* ImageTag */
    uint32_t cbSize = le32(hdr + 8);
    if (cbSize < 208) goto fail;
    uint32_t flags = le32(hdr + 0x10);
    w->chunk_size  = le32(hdr + 0x14);
    if (w->chunk_size == 0) w->chunk_size = 32768;
    w->image_count = le32(hdr + 0x2C);
    /* rhOffsetTable @ 0x30, rhXmlData @ 0x48, rhBootMetadata @ 0x60,
       dwBootIndex @ 0x78, rhIntegrity @ 0x7C. */
    parse_reshdr(hdr + 0x30, &w->offset_table_res);
    w->boot_index = le32(hdr + 0x78);

    if (!(flags & WIM_FLAG_COMPRESSION))      w->comp = WIM_COMP_NONE;
    else if (flags & WIM_FLAG_COMP_LZMS)      w->comp = WIM_COMP_LZMS;
    else if (flags & WIM_FLAG_COMP_LZX)       w->comp = WIM_COMP_LZX;
    else if (flags & WIM_FLAG_COMP_XPRESS)    w->comp = WIM_COMP_XPRESS;
    else                                      w->comp = WIM_COMP_NONE;

    /* The offset/lookup table. It is itself a resource; in practice it is
       stored UNCOMPRESSED even in compressed WIMs. If it ever isn't, the
       resource reader (resource.c) will be wired in; for now require raw. */
    if (w->offset_table_res.flags & WIM_RESHDR_COMPRESSED) goto fail;  /* TODO via resource.c */
    uint64_t tbl_size = w->offset_table_res.size_in_wim;
    if (tbl_size == 0 || tbl_size % LOOKUP_ENTRY_SIZE != 0) goto fail;
    size_t n = (size_t)(tbl_size / LOOKUP_ENTRY_SIZE);
    uint8_t *tbl = malloc((size_t)tbl_size);
    if (!tbl) goto fail;
    if (wim_pread(w, tbl, (size_t)tbl_size, w->offset_table_res.offset) != 0) { free(tbl); goto fail; }

    w->res = calloc(n, sizeof(wim_resource_t));
    if (!w->res) { free(tbl); goto fail; }
    for (size_t i = 0; i < n; i++) {
        const uint8_t *e = tbl + i * LOOKUP_ENTRY_SIZE;
        wim_resource_t *r = &w->res[i];
        parse_reshdr(e, r);                       /* 24 bytes */
        r->part_number = le16(e + 24);
        r->refcount    = le32(e + 26);
        memcpy(r->hash, e + 30, 20);
    }
    w->nres = n;
    free(tbl);
    return w;
fail:
    close(fd);
    free(w->res);
    free(w);
    return NULL;
}

void wim_close(wim_t *w) {
    if (!w) return;
    if (w->fd >= 0) close(w->fd);
    free(w->res);
    free(w);
}

wim_comp_t wim_compression(const wim_t *w) { return w->comp; }
uint32_t   wim_chunk_size(const wim_t *w)  { return w->chunk_size; }
uint32_t   wim_image_count(const wim_t *w) { return w->image_count; }
uint32_t   wim_boot_index(const wim_t *w)  { return w->boot_index; }
size_t     wim_num_resources(const wim_t *w) { return w->nres; }
const wim_resource_t *wim_resource(const wim_t *w, size_t i) {
    return i < w->nres ? &w->res[i] : NULL;
}

const wim_resource_t *wim_metadata_resource(const wim_t *w, uint32_t image_index) {
    uint32_t seen = 0;
    for (size_t i = 0; i < w->nres; i++) {
        if (w->res[i].flags & WIM_RESHDR_METADATA) {
            if (++seen == image_index) return &w->res[i];
        }
    }
    return NULL;
}

int wim_read_uncompressed(const wim_t *w, const wim_resource_t *r, uint8_t *out) {
    if (r->flags & WIM_RESHDR_COMPRESSED) return -1;     /* needs the chunk reader */
    if (r->size_in_wim != r->orig_size) return -1;        /* uncompressed: stored == orig */
    return wim_pread(w, out, (size_t)r->orig_size, r->offset);
}

const wim_resource_t *wim_lookup_by_hash(const wim_t *w, const uint8_t hash[20]) {
    static const uint8_t zero[20] = { 0 };
    if (memcmp(hash, zero, 20) == 0) return NULL;
    for (size_t i = 0; i < w->nres; i++)
        if (memcmp(w->res[i].hash, hash, 20) == 0) return &w->res[i];
    return NULL;
}
