/*
 * wimimg.c -- WIM image metadata parser. See wimimg.h for the on-disk layout.
 */
#include "wimimg.h"
#include <stdlib.h>
#include <string.h>

struct wim_image {
    uint8_t  *meta;         /* decompressed metadata resource */
    uint64_t  meta_size;
    uint64_t  root_offset;  /* offset of the root dentry */
    /* SECURITYDATA */
    uint32_t  sd_count;
    uint64_t *sd_off;       /* sd_off[i] = offset of descriptor i within meta */
    uint64_t *sd_len;       /* sd_len[i] = byte length of descriptor i */
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v;
}
static uint64_t align8(uint64_t x) { return (x + 7) & ~(uint64_t)7; }

wim_image_t *wim_image_open(wim_t *w, uint32_t image_index) {
    const wim_resource_t *md = wim_metadata_resource(w, image_index);
    if (!md || md->orig_size < 8) return NULL;

    wim_image_t *img = calloc(1, sizeof *img);
    if (!img) return NULL;
    img->meta_size = md->orig_size;
    img->meta = malloc((size_t)md->orig_size);
    if (!img->meta) { free(img); return NULL; }
    if (wim_read_resource(w, md, img->meta) != 0) { wim_image_close(img); return NULL; }

    /* SECURITYDATA: u32 total_length, u32 num_entries, u64[num_entries] sizes,
     * then the descriptors; the dentry tree starts at align8(total_length). */
    uint32_t total = rd32(img->meta);
    uint32_t ne    = rd32(img->meta + 4);
    if ((uint64_t)8 + (uint64_t)ne * 8 > img->meta_size) { wim_image_close(img); return NULL; }
    img->sd_count = ne;
    if (ne) {
        img->sd_off = malloc(ne * sizeof(uint64_t));
        img->sd_len = malloc(ne * sizeof(uint64_t));
        if (!img->sd_off || !img->sd_len) { wim_image_close(img); return NULL; }
        uint64_t cur = 8 + (uint64_t)ne * 8;     /* descriptors begin right after the size array */
        for (uint32_t i = 0; i < ne; i++) {
            uint64_t len = rd64(img->meta + 8 + (uint64_t)i * 8);
            img->sd_off[i] = cur;
            img->sd_len[i] = len;
            cur += len;
            if (cur > img->meta_size) { wim_image_close(img); return NULL; }
        }
    }
    img->root_offset = align8(total);
    if (img->root_offset + 8 > img->meta_size) { wim_image_close(img); return NULL; }
    return img;
}

void wim_image_close(wim_image_t *img) {
    if (!img) return;
    free(img->meta); free(img->sd_off); free(img->sd_len); free(img);
}

const uint8_t *wim_image_meta(const wim_image_t *img, uint64_t *size_out) {
    if (size_out) *size_out = img->meta_size;
    return img->meta;
}

uint64_t wim_image_root_offset(const wim_image_t *img) { return img->root_offset; }

int wim_dentry_at(const wim_image_t *img, uint64_t off, wim_dentry_t *d) {
    const uint8_t *m = img->meta;
    uint64_t n = img->meta_size;
    if (off + 8 > n) return -1;
    uint64_t length = rd64(m + off);
    if (length == 0) return 0;                    /* end of sibling list */
    if (length < 102 || off + length > n) return -1;

    memset(d, 0, sizeof *d);
    d->self_offset = off;
    d->length      = length;
    d->attributes  = rd32(m + off + 8);
    d->security_id = (int32_t)rd32(m + off + 12);
    d->subdir_offset = rd64(m + off + 16);
    d->creation_time    = rd64(m + off + 40);
    d->last_access_time = rd64(m + off + 48);
    d->last_write_time  = rd64(m + off + 56);
    memcpy(d->hash, m + off + 64, 20);
    if (d->attributes & WIM_FILE_ATTRIBUTE_REPARSE_POINT)
        d->reparse_tag = rd32(m + off + 88);
    else
        d->hard_link_group_id = rd64(m + off + 88);
    d->num_ads           = rd16(m + off + 96);
    d->short_name_nbytes = rd16(m + off + 98);
    d->file_name_nbytes  = rd16(m + off + 100);

    uint64_t p = off + 102;
    if (d->file_name_nbytes) {
        if (p + d->file_name_nbytes + 2 > off + length) return -1;
        d->file_name = m + p;
        p += d->file_name_nbytes + 2;             /* +2 UTF-16 null terminator */
    }
    if (d->short_name_nbytes) {
        if (p + d->short_name_nbytes + 2 > off + length) return -1;
        d->short_name = m + p;
        p += d->short_name_nbytes + 2;
    }
    d->ads_offset = align8(p);                     /* ADS entries start 8-aligned after names */

    /* Walk the ADS entries to find where the tagged-item region begins. */
    uint64_t a = d->ads_offset, rec_end = off + length;
    for (uint16_t k = 0; k < d->num_ads; k++) {
        if (a + 38 > rec_end) { a = rec_end; break; }
        uint64_t alen = rd64(m + a);
        a = align8(a + (alen ? alen : 38));
        if (a > rec_end) { a = rec_end; break; }
    }
    d->tagged_offset = a;
    return 1;
}

int wim_dentry_tagged_next(const wim_image_t *img, const wim_dentry_t *d,
                           uint64_t *iter, wim_tagged_t *t) {
    const uint8_t *m = img->meta;
    uint64_t end = d->self_offset + d->length;
    uint64_t off = *iter;
    if (off + 8 > end) return 0;                    /* no room for another header */
    uint32_t tag = rd32(m + off);
    uint32_t len = rd32(m + off + 4);
    if (off + 8 + len > end) return -1;
    t->tag = tag;
    t->length = len;
    t->data = m + off + 8;
    *iter = align8(off + 8 + len);
    return 1;
}

int wim_dentry_ads(const wim_image_t *img, const wim_dentry_t *d, uint16_t i, wim_ads_t *a) {
    const uint8_t *m = img->meta;
    uint64_t n = img->meta_size;
    if (i >= d->num_ads) return -1;
    /* ADS entry layout: u64 length, u64 reserved, u8 hash[20], u16 name_nbytes,
     * name UTF-16LE + u16 null, pad to 8. */
    uint64_t off = d->ads_offset;
    for (uint16_t k = 0; k <= i; k++) {
        if (off + 38 > n) return -1;
        uint64_t len = rd64(m + off);
        uint64_t reclen = len ? len : 38;          /* a 0-length record still occupies the fixed part */
        if (k == i) {
            memset(a, 0, sizeof *a);
            a->length = len;
            memcpy(a->hash, m + off + 16, 20);
            a->name_nbytes = rd16(m + off + 36);
            if (a->name_nbytes) {
                if (off + 38 + a->name_nbytes > n) return -1;
                a->name = m + off + 38;
            }
            return 0;
        }
        off = align8(off + reclen);
    }
    return -1;
}

const uint8_t *wim_image_sd(const wim_image_t *img, int32_t security_id, uint64_t *len_out) {
    if (security_id < 0 || (uint32_t)security_id >= img->sd_count) return NULL;
    if (len_out) *len_out = img->sd_len[security_id];
    return img->meta + img->sd_off[security_id];
}

uint32_t wim_image_sd_count(const wim_image_t *img) { return img->sd_count; }
