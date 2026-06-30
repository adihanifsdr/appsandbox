/* gpt.c -- see gpt.h. */

#include "gpt.h"
#include <stdlib.h>
#include <string.h>

/* Canonical partition type GUIDs, in on-disk (mixed-endian) byte order. */
static const uint8_t TYPE_ESP[16] = {   /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11, 0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B };
static const uint8_t TYPE_MSR[16] = {   /* E3C9E316-0B5C-4DB8-817D-F92DF00215AE */
    0x16,0xE3,0xC9,0xE3, 0x5C,0x0B, 0xB8,0x4D, 0x81,0x7D, 0xF9,0x2D,0xF0,0x02,0x15,0xAE };
static const uint8_t TYPE_WIN[16] = {   /* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 */
    0xA2,0xA0,0xD0,0xEB, 0xE5,0xB9, 0x33,0x44, 0x87,0xC0, 0x68,0xB6,0xB7,0x26,0x99,0xC7 };

static uint32_t crc32(const void *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static void rand_guid(uint8_t g[16]) {
    arc4random_buf(g, 16);
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);   /* version 4 */
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);   /* variant 1 */
}

static void put_le16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put_le32(uint8_t *p, uint32_t v) { for (int i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static void put_le64(uint8_t *p, uint64_t v) { for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }

/* Fill one 128-byte partition entry. name is ASCII -> UTF-16LE. */
static void fill_entry(uint8_t *e, const uint8_t type[16],
                       uint64_t first, uint64_t last, const char *name) {
    memcpy(e, type, 16);
    rand_guid(e + 16);
    put_le64(e + 32, first);
    put_le64(e + 40, last);
    put_le64(e + 48, 0);                       /* attributes */
    for (int i = 0; name && name[i] && i < 35; i++) put_le16(e + 56 + i*2, (uint16_t)name[i]);
}

int gpt_write_windows(blockio_t *io, uint32_t ss,
                      uint64_t esp_mb, uint64_t msr_mb, gpt_layout_t *out) {
    if (ss != 512) return -1;                   /* layout assumes 512-byte LBAs */
    uint64_t total = blockio_size(io) / ss;
    if (total < 2048 + 64) return -1;

    const uint32_t NENT = 128, ESZ = 128;
    uint32_t arr_sectors = (NENT * ESZ + ss - 1) / ss;   /* 32 */
    uint64_t align = (1024 * 1024) / ss;                 /* 2048 */

    uint64_t primary_hdr = 1;
    uint64_t primary_arr = 2;
    uint64_t backup_hdr  = total - 1;
    uint64_t backup_arr  = total - 1 - arr_sectors;
    uint64_t first_usable = primary_arr + arr_sectors;   /* 34 */
    uint64_t last_usable  = backup_arr - 1;

    /* Partition LBA ranges (1 MiB aligned). */
    uint64_t esp_start = (first_usable + align - 1) / align * align;
    uint64_t esp_count = esp_mb * 1024 * 1024 / ss;
    uint64_t msr_start = esp_start + esp_count;
    uint64_t msr_count = msr_mb * 1024 * 1024 / ss;
    uint64_t win_start = msr_start + msr_count;
    if (win_start > last_usable) return -1;
    uint64_t win_count = (last_usable + 1) - win_start;   /* fill remainder */

    /* Build the 128-entry array (only first 3 used). */
    uint8_t *arr = calloc(NENT, ESZ);
    if (!arr) return -1;
    fill_entry(arr + 0*ESZ, TYPE_ESP, esp_start, esp_start + esp_count - 1, "EFI system partition");
    fill_entry(arr + 1*ESZ, TYPE_MSR, msr_start, msr_start + msr_count - 1, "Microsoft reserved partition");
    fill_entry(arr + 2*ESZ, TYPE_WIN, win_start, win_start + win_count - 1, "Basic data partition");
    uint32_t arr_crc = crc32(arr, NENT * ESZ);

    uint8_t disk_guid[16]; rand_guid(disk_guid);

    /* Header builder (used for primary + backup; my_lba/alt_lba/arr_lba differ). */
    uint8_t hdr[512];
    int rc = -1;
    for (int pass = 0; pass < 2; pass++) {
        memset(hdr, 0, sizeof hdr);
        uint64_t my_lba  = pass == 0 ? primary_hdr : backup_hdr;
        uint64_t alt_lba = pass == 0 ? backup_hdr  : primary_hdr;
        uint64_t arr_lba = pass == 0 ? primary_arr : backup_arr;
        memcpy(hdr, "EFI PART", 8);
        put_le32(hdr + 8, 0x00010000);          /* revision 1.0 */
        put_le32(hdr + 12, 92);                 /* header size */
        /* hdr+16 header_crc = 0 for now */
        put_le64(hdr + 24, my_lba);
        put_le64(hdr + 32, alt_lba);
        put_le64(hdr + 40, first_usable);
        put_le64(hdr + 48, last_usable);
        memcpy(hdr + 56, disk_guid, 16);
        put_le64(hdr + 72, arr_lba);
        put_le32(hdr + 80, NENT);
        put_le32(hdr + 84, ESZ);
        put_le32(hdr + 88, arr_crc);
        put_le32(hdr + 16, crc32(hdr, 92));     /* header CRC over 92 bytes */

        if (blockio_write(io, my_lba * ss, hdr, 512) != 0) goto done;
        if (blockio_write(io, arr_lba * ss, arr, NENT * ESZ) != 0) goto done;
    }

    /* Protective MBR (LBA 0): one 0xEE partition covering the whole disk. */
    uint8_t mbr[512]; memset(mbr, 0, sizeof mbr);
    uint8_t *pe = mbr + 446;
    pe[0] = 0x00;                               /* not bootable */
    pe[1] = 0x00; pe[2] = 0x02; pe[3] = 0x00;   /* CHS first */
    pe[4] = 0xEE;                               /* type: GPT protective */
    pe[5] = 0xFF; pe[6] = 0xFF; pe[7] = 0xFF;   /* CHS last */
    put_le32(pe + 8, 1);                        /* first LBA */
    uint64_t prot = total - 1;
    put_le32(pe + 12, prot > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)prot);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    if (blockio_write(io, 0, mbr, 512) != 0) goto done;

    out->sector_size = ss;
    out->esp_start_lba = esp_start; out->esp_count_lba = esp_count;
    out->msr_start_lba = msr_start; out->msr_count_lba = msr_count;
    out->win_start_lba = win_start; out->win_count_lba = win_count;
    memcpy(out->disk_guid,     disk_guid,        16);
    memcpy(out->esp_part_guid, arr + 0*ESZ + 16, 16);   /* ESP unique GUID */
    memcpy(out->win_part_guid, arr + 2*ESZ + 16, 16);   /* Windows unique GUID */
    rc = 0;
done:
    free(arr);
    return rc;
}
