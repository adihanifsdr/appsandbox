/*
 * wimimg -- WIM image metadata parser (the layer above wim.c/resource.c).
 *
 * Decompresses one image's metadata resource (LZX/XPRESS/LZMS via resource.c)
 * and exposes its SECURITYDATA table + DIRENTRY tree so the NTFS writer can
 * walk the file tree, pull each file's data blob by SHA-1, and reapply the
 * per-file security descriptor.
 *
 * On-disk DIRENTRY layout (confirmed by inspecting a real install.wim, version
 * 0x00010d00 -- the classic WIM_VERSION_DEFAULT format with inline stream hash):
 *   0x00 u64 length            total record bytes incl names, 8-aligned; 0 = end of sibling list
 *   0x08 u32 attributes        FILE_ATTRIBUTE_* bitmask
 *   0x0C s32 security_id       index into SECURITYDATA (-1 = none)
 *   0x10 u64 subdir_offset     offset (in metadata buffer) of child list; 0 = none
 *   0x18 u64 unused1
 *   0x20 u64 unused2
 *   0x28 u64 creation_time     FILETIME
 *   0x30 u64 last_access_time  FILETIME
 *   0x38 u64 last_write_time   FILETIME
 *   0x40 u8[20] hash           SHA-1 of the unnamed data stream (0 = empty/dir)
 *   0x54 u32 unused3
 *   0x58 (union) reparse_tag u32 @0x58 if REPARSE_POINT, else hard_link_group_id u64 @0x58
 *   0x60 u16 num_extra_streams (ADS count)
 *   0x62 u16 short_name_nbytes (bytes, excl null)
 *   0x64 u16 file_name_nbytes  (bytes, excl null)
 *   0x66 file_name UTF-16LE + u16 null; then short_name UTF-16LE + u16 null; pad to 8
 *   then num_extra_streams ADS entries (each 8-aligned)
 */
#ifndef ASB_WIMIMG_H
#define ASB_WIMIMG_H

#include "wim.h"
#include <stdint.h>

#define WIM_FILE_ATTRIBUTE_READONLY      0x00000001
#define WIM_FILE_ATTRIBUTE_HIDDEN        0x00000002
#define WIM_FILE_ATTRIBUTE_SYSTEM        0x00000004
#define WIM_FILE_ATTRIBUTE_DIRECTORY     0x00000010
#define WIM_FILE_ATTRIBUTE_ARCHIVE       0x00000020
#define WIM_FILE_ATTRIBUTE_REPARSE_POINT 0x00000400

typedef struct wim_image wim_image_t;

typedef struct {
    uint64_t self_offset;          /* offset of this dentry in the metadata buffer */
    uint64_t length;
    uint32_t attributes;
    int32_t  security_id;
    uint64_t subdir_offset;
    uint64_t creation_time, last_access_time, last_write_time;
    uint8_t  hash[20];             /* SHA-1 of unnamed stream */
    uint32_t reparse_tag;          /* valid iff attributes & REPARSE_POINT */
    uint64_t hard_link_group_id;   /* valid iff !REPARSE_POINT */
    uint16_t num_ads;
    const uint8_t *file_name;      /* UTF-16LE, file_name_nbytes long (not null-term'd here) */
    uint16_t file_name_nbytes;
    const uint8_t *short_name;     /* UTF-16LE 8.3 name, or NULL */
    uint16_t short_name_nbytes;
    uint64_t ads_offset;           /* metadata offset where ADS entries begin */
    uint64_t tagged_offset;        /* metadata offset where tagged items begin (after ADS) */
} wim_dentry_t;

/* A "tagged item" appended to a dentry after its names + ADS entries:
 *   u32 tag, u32 length, u8 data[length], pad to 8.
 * Known tags: 0x00000001 object id, 0x00000002 Windows EAs (e.g.
 * $CI.CATALOGHINT / $KERNEL.* code-integrity hints), others. */
#define WIM_TAGGED_OBJECT_ID  0x00000001
#define WIM_TAGGED_EA         0x00000002
typedef struct {
    uint32_t tag;
    uint32_t length;               /* data length (excl the 8-byte header) */
    const uint8_t *data;
} wim_tagged_t;

typedef struct {
    uint64_t length;               /* ADS record length (8-aligned) */
    uint8_t  hash[20];             /* SHA-1 of this stream's data */
    const uint8_t *name;           /* UTF-16LE stream name, or NULL for unnamed */
    uint16_t name_nbytes;
} wim_ads_t;

/* Open image `image_index` (1-based): decompress its metadata resource and parse
 * the SECURITYDATA header. NULL on error. */
wim_image_t *wim_image_open(wim_t *w, uint32_t image_index);
void         wim_image_close(wim_image_t *img);

const uint8_t *wim_image_meta(const wim_image_t *img, uint64_t *size_out);
uint64_t       wim_image_root_offset(const wim_image_t *img);  /* root dentry offset */

/* Parse the dentry at metadata offset `off`. Returns 1 + fills `d` on success;
 * 0 if length==0 (end of sibling list); -1 on malformed/overrun. */
int wim_dentry_at(const wim_image_t *img, uint64_t off, wim_dentry_t *d);

/* Parse ADS entry `i` (0-based) of `d`. Returns 0/-1. */
int wim_dentry_ads(const wim_image_t *img, const wim_dentry_t *d, uint16_t i, wim_ads_t *a);

/* Iterate tagged items. Start with `*iter = d->tagged_offset`. Returns 1 and
 * fills `t` (advancing `*iter`) per item; 0 when the region (up to the dentry's
 * end) is exhausted; -1 on overrun. */
int wim_dentry_tagged_next(const wim_image_t *img, const wim_dentry_t *d,
                           uint64_t *iter, wim_tagged_t *t);

/* Security descriptor for `security_id` (its self-relative bytes). NULL if id<0
 * or out of range. */
const uint8_t *wim_image_sd(const wim_image_t *img, int32_t security_id, uint64_t *len_out);
uint32_t       wim_image_sd_count(const wim_image_t *img);

#endif /* ASB_WIMIMG_H */
