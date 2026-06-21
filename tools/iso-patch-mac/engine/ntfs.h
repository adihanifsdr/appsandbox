/*
 * ntfs -- NTFS volume WRITER (the ext4.c analog).
 *
 * Authors a UEFI-bootable NTFS volume byte-by-byte into a raw image via the
 * blockio coalescer, with no external libraries. Three-phase, mirroring
 * ext4.c: ntfs_writer_open() plans the layout (cluster size, $MFT/$MFTMirr
 * LCNs) and pre-creates the 16 system metafiles; ntfs_add_dir/ntfs_add_file
 * buffer a FILE record into an in-memory MFT + record the entry in the parent
 * directory's $I30 index, streaming non-resident $DATA straight to disk;
 * ntfs_writer_close() serializes $MFT (+ $MFTMirr), every directory index
 * (with USA fixups), $Bitmap, $Secure, $UpCase, $Boot (+ backup) and the
 * 0xFF $LogFile.
 *
 * Scope (v1, per docs/appsandbox/NTFS-WIM-DESIGN.md §5): single contiguous
 * $DATA run per file, NTFS 3.1, names emitted as ns=3 (WIN32&DOS) when 8.3-
 * legal else a WIN32+DOS pair, $Secure interning a small descriptor set.
 */
#ifndef ASB_NTFS_H
#define ASB_NTFS_H

#include "blockio.h"
#include <stdint.h>

/* ---- NTFS on-disk constants ---- */
#define NTFS_SECTOR        512
#define NTFS_MFT_RECSZ     1024            /* fixed FILE-record size */
#define NTFS_INDX_SIZE     4096            /* INDX block / index record size */

/* Attribute type codes. */
#define NTFS_AT_STANDARD_INFORMATION 0x10
#define NTFS_AT_ATTRIBUTE_LIST       0x20
#define NTFS_AT_FILE_NAME            0x30
#define NTFS_AT_OBJECT_ID            0x40
#define NTFS_AT_SECURITY_DESCRIPTOR  0x50
#define NTFS_AT_VOLUME_NAME          0x60
#define NTFS_AT_VOLUME_INFORMATION   0x70
#define NTFS_AT_DATA                 0x80
#define NTFS_AT_INDEX_ROOT           0x90
#define NTFS_AT_INDEX_ALLOCATION     0xA0
#define NTFS_AT_BITMAP               0xB0
#define NTFS_AT_REPARSE_POINT        0xC0
#define NTFS_AT_EA_INFORMATION       0xD0
#define NTFS_AT_EA                   0xE0
#define NTFS_AT_END                  0xFFFFFFFF

/* FILE-record flags. */
#define NTFS_FR_IN_USE     0x0001
#define NTFS_FR_DIRECTORY  0x0002

/* DOS / Win32 file attributes (match the WIM dentry attribute bits). */
#define NTFS_FA_READONLY   0x00000001
#define NTFS_FA_HIDDEN     0x00000002
#define NTFS_FA_SYSTEM     0x00000004
#define NTFS_FA_DIRECTORY  0x10000000   /* NTFS-internal "is directory" bit in $STD_INFO/$FILE_NAME flags */
#define NTFS_FA_ARCHIVE    0x00000020
#define NTFS_FA_REPARSE    0x00000400

/* Reserved metafile record numbers. */
enum {
    NTFS_REC_MFT = 0, NTFS_REC_MFTMIRR, NTFS_REC_LOGFILE, NTFS_REC_VOLUME,
    NTFS_REC_ATTRDEF, NTFS_REC_ROOT, NTFS_REC_BITMAP, NTFS_REC_BOOT,
    NTFS_REC_BADCLUS, NTFS_REC_SECURE, NTFS_REC_UPCASE, NTFS_REC_EXTEND,
    NTFS_REC_RESERVED12, NTFS_REC_RESERVED13, NTFS_REC_RESERVED14, NTFS_REC_RESERVED15,
    NTFS_REC_FIRST_USER = 16
};

typedef struct ntfs_writer ntfs_writer_t;

/* Plan the layout over [part_lba, part_lba+part_sectors) of the image and
 * pre-create metafiles 0-15. `label` is UTF-8 (may be NULL). NULL on error. */
ntfs_writer_t *ntfs_writer_open(blockio_t *io, uint64_t part_lba, uint64_t part_sectors,
                                const char *label);

/* MFT reference of the root directory (record 5), with its sequence number in
 * the high 16 bits — the value to pass as `parent_ref` for top-level entries. */
uint64_t ntfs_root_ref(const ntfs_writer_t *w);

/* Intern a self-relative security descriptor; returns its NTFS security_id
 * (>= 0x100). Identical descriptors collapse to one id. Pass NULL/0 to get the
 * shared default descriptor's id. */
int32_t ntfs_secure_intern(ntfs_writer_t *w, const void *sd, uint32_t sd_len);

/* Add a directory under `parent_ref`. Name is UTF-16LE, `name_chars` code units.
 * `short_name`/`short_name_chars` is the WIM-supplied DOS 8.3 alias (NULL/0 if
 * none), and the namespace follows the NTFS format: no alias -> a
 * single POSIX(ns=0) name; alias present -> WIN32(ns=1) long + DOS(ns=2) short
 * pair (both indexed in the parent, link_count counts both), collapsing to a
 * single WIN32_AND_DOS(ns=3) entry only when long and short are equal under
 * $UpCase. Returns the new dir's MFT ref (record#|sequence<<48) or 0. Times are
 * Windows FILETIME. */
uint64_t ntfs_add_dir(ntfs_writer_t *w, uint64_t parent_ref,
                      const uint16_t *name, int name_chars,
                      const uint16_t *short_name, int short_name_chars,
                      uint32_t attributes, int32_t security_id,
                      uint64_t ctime, uint64_t atime, uint64_t mtime);

/* Add a regular file under `parent_ref`. `data` (may be NULL iff size==0) holds
 * the entire unnamed $DATA stream; resident if it fits, else a single
 * contiguous non-resident run streamed to disk now. `short_name`/`short_name_chars`
 * is the DOS 8.3 alias (see ntfs_add_dir). Returns the MFT ref or 0. */
uint64_t ntfs_add_file(ntfs_writer_t *w, uint64_t parent_ref,
                       const uint16_t *name, int name_chars,
                       const uint16_t *short_name, int short_name_chars,
                       uint32_t attributes, int32_t security_id,
                       uint64_t ctime, uint64_t atime, uint64_t mtime,
                       const void *data, uint64_t size);

/* Like ntfs_add_file but DEFERS the $DATA write: builds the record, allocates a
 * single contiguous non-resident run for `size` bytes, records the run, and
 * returns via *out_byte_off the ABSOLUTE image byte offset where exactly `size`
 * bytes of file content must be written later (the last cluster's slack is left
 * as the image's natural zeros). `size` MUST be large enough to be non-resident
 * (caller passes size>700). No bytes are written to disk by this call, so a
 * worker thread can fill *out_byte_off concurrently via blockio_pwrite while the
 * main thread keeps building metadata. Returns the MFT ref or 0 (out_byte_off=0
 * on failure). Cluster allocation is sequential/deterministic, so the resulting
 * layout is identical to the single-threaded ntfs_add_file path. */
uint64_t ntfs_add_file_deferred(ntfs_writer_t *w, uint64_t parent_ref,
                       const uint16_t *name, int name_chars,
                       const uint16_t *short_name, int short_name_chars,
                       uint32_t attributes, int32_t security_id,
                       uint64_t ctime, uint64_t atime, uint64_t mtime,
                       uint64_t size, uint64_t *out_byte_off);

/* Add a hard link (new name under `parent_ref`) for the existing inode
 * `target_ref` returned by ntfs_add_file -- shares the inode/content instead of
 * duplicating it (the NTFS hard-link model). `size` is the inode's $DATA size.
 * Returns target_ref, or 0 if the target record can't fit another name (caller
 * should then create a separate record). */
uint64_t ntfs_add_hardlink(ntfs_writer_t *w, uint64_t target_ref, uint64_t parent_ref,
                           const uint16_t *name, int name_chars,
                           const uint16_t *short_name, int short_name_chars,
                           uint32_t attributes, uint64_t ctime, uint64_t atime, uint64_t mtime,
                           uint64_t size);

/* Apply the WIM root directory's own security_id to the root record (the root
 * dir gets its real SD rather than the default). Call before ntfs_writer_close. */
void ntfs_set_root_security(ntfs_writer_t *w, int32_t sec_id);

/* Attach a reparse point (junction/symlink/etc.) to the record `ref` returned by
 * ntfs_add_dir/ntfs_add_file. `data`/`data_len` is the tag-specific reparse buffer
 * exactly as stored in the WIM (bytes after the 8-byte REPARSE_DATA_BUFFER header);
 * the writer prepends tag|len|reserved and emits $REPARSE_POINT at close. The
 * record's attributes must keep FILE_ATTRIBUTE_REPARSE_POINT (0x400) set. */
void ntfs_set_reparse(ntfs_writer_t *w, uint64_t ref, uint32_t tag,
                      const void *data, uint32_t data_len);

/* Serialize all deferred structures. Returns 0 on success. */
int ntfs_writer_close(ntfs_writer_t *w);

#endif /* ASB_NTFS_H */
