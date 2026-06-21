/*
 * fat32 -- minimal FAT32 writer for the EFI System Partition.
 *
 * Authors the smallest correct FAT32 a UEFI firmware will mount: BPB + FSInfo +
 * two FATs + a directory tree + file data, all 8.3 names (the boot files --
 * \EFI\BOOT\BOOTAA64.EFI, \EFI\Microsoft\Boot\{BOOTMGFW.EFI,BCD} -- are all
 * 8.3-legal, so no long-filename entries are needed for boot). Same three-phase
 * shape as ntfs.c: open() plans geometry, addfile/mkdir buffer the tree and
 * stream file clusters, close() writes the FATs + directory clusters + boot
 * sectors. All I/O routes through the blockio coalescer.
 */
#ifndef ASB_FAT32_H
#define ASB_FAT32_H

#include "blockio.h"
#include <stdint.h>

typedef struct fat32_writer fat32_writer_t;

/* Plan a FAT32 volume over [part_lba, part_lba+part_sectors). label is up to 11
 * chars (copied, upper-cased). NULL on error. */
fat32_writer_t *fat32_open(blockio_t *io, uint64_t part_lba, uint64_t part_sectors,
                           const char *label);

/* Create a directory (and any missing parents). Path uses '/' or '\\'
 * separators; each component must be 8.3-legal. Returns 0/-1. */
int fat32_mkdir(fat32_writer_t *w, const char *path);

/* Create a file with the given contents (parents auto-created). Streams the
 * data into a contiguous cluster run now. Returns 0/-1. */
int fat32_addfile(fat32_writer_t *w, const char *path, const void *data, uint64_t size);

/* Serialize FATs, directory clusters, and boot sectors. Returns 0/-1. */
int fat32_close(fat32_writer_t *w);

#endif /* ASB_FAT32_H */
