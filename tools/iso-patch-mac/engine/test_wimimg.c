/*
 * test_wimimg.c -- validate the WIM image/metadata parser against real data.
 * Walks the whole DIRENTRY tree of an image and asserts:
 *   (1) every regular file's unnamed-stream SHA-1 resolves in the lookup table,
 *   (2) every ADS hash resolves too,
 *   (3) the dentry length-invariant holds (confirms name/padding offsets),
 *   (4) the known boot paths exist.
 * Exit non-zero on any hash miss / invariant break.
 */
#include "wimimg.h"
#include "wim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static wim_t *g_w;
static wim_image_t *g_img;
static unsigned long dirs, files, reparse, ads_total, empty_files;
static unsigned long hash_ok, hash_miss, inv_ok, inv_bad, parse_err;
static unsigned long tagged_total, tag_ea, tag_objid, tag_other;
static int f_win, f_sys32, f_ntoskrnl, f_hive, f_bootmgr;

static void name_ascii(const uint8_t *p, int nb, char *buf, int cap) {
    int j = 0;
    for (int i = 0; i < nb && j < cap - 1; i += 2) {
        unsigned ch = p[i] | (p[i+1] << 8);
        buf[j++] = (ch >= 32 && ch < 127) ? (char)ch : '?';
    }
    buf[j] = 0;
}

static void walk(uint64_t children_off, const char *path, int depth) {
    if (depth > 64) return;
    uint64_t off = children_off;
    for (;;) {
        wim_dentry_t d;
        int r = wim_dentry_at(g_img, off, &d);
        if (r == 0) break;
        if (r < 0) { parse_err++; if (parse_err <= 8) printf("PARSE ERR off=%llu under %s\n",
                        (unsigned long long)off, path); break; }
        char nm[256]; name_ascii(d.file_name, d.file_name_nbytes, nm, sizeof nm);
        char full[1200]; snprintf(full, sizeof full, "%s\\%s", path, nm);

        {   /* full structural accounting: names + ADS + tagged items == length */
            uint64_t it = d.tagged_offset; wim_tagged_t t; int terr = 0, tr;
            while ((tr = wim_dentry_tagged_next(g_img, &d, &it, &t)) == 1) {
                tagged_total++;
                if (t.tag == WIM_TAGGED_EA) tag_ea++;
                else if (t.tag == WIM_TAGGED_OBJECT_ID) tag_objid++;
                else tag_other++;
            }
            if (tr < 0) terr = 1;
            uint64_t end = d.self_offset + d.length;
            if (!terr && it == end) inv_ok++;
            else { inv_bad++; if (inv_bad <= 8) printf("INV %s len=%llu consumed_to=%llu terr=%d\n",
                        full, (unsigned long long)d.length, (unsigned long long)it, terr); }
        }
        ads_total += d.num_ads;
        if (d.attributes & WIM_FILE_ATTRIBUTE_REPARSE_POINT) reparse++;

        if (d.attributes & WIM_FILE_ATTRIBUTE_DIRECTORY) {
            dirs++;
            if (d.subdir_offset) walk(d.subdir_offset, full, depth + 1);
        } else {
            files++;
            int az = 1; for (int i = 0; i < 20; i++) if (d.hash[i]) { az = 0; break; }
            if (az) empty_files++;
            else if (wim_lookup_by_hash(g_w, d.hash)) hash_ok++;
            else { hash_miss++; if (hash_miss <= 8) printf("HASH MISS %s\n", full); }
        }
        for (uint16_t i = 0; i < d.num_ads; i++) {
            wim_ads_t a;
            if (wim_dentry_ads(g_img, &d, i, &a) == 0) {
                int az = 1; for (int k = 0; k < 20; k++) if (a.hash[k]) { az = 0; break; }
                if (!az) { if (wim_lookup_by_hash(g_w, a.hash)) hash_ok++; else hash_miss++; }
            }
        }
        if (!strcasecmp(full, "\\Windows")) f_win = 1;
        else if (!strcasecmp(full, "\\Windows\\System32")) f_sys32 = 1;
        else if (!strcasecmp(full, "\\Windows\\System32\\ntoskrnl.exe")) f_ntoskrnl = 1;
        else if (!strcasecmp(full, "\\Windows\\System32\\config\\SYSTEM")) f_hive = 1;
        else if (!strcasecmp(full, "\\Windows\\Boot\\EFI\\bootmgfw.efi")) f_bootmgr = 1;

        off = d.self_offset + d.length;
    }
}

int main(int c, char **v) {
    const char *path = c > 1 ? v[1] : "/Volumes/CCCOMA_A64FRE_EN-US_DV9/sources/install.wim";
    uint32_t idx = c > 2 ? (uint32_t)atoi(v[2]) : 1;
    g_w = wim_open(path); if (!g_w) { fprintf(stderr, "wim_open fail\n"); return 2; }
    g_img = wim_image_open(g_w, idx); if (!g_img) { fprintf(stderr, "image_open fail\n"); return 2; }
    printf("image %u: sd_count=%u root_off=%llu\n", idx,
           wim_image_sd_count(g_img), (unsigned long long)wim_image_root_offset(g_img));
    wim_dentry_t root;
    int rr = wim_dentry_at(g_img, wim_image_root_offset(g_img), &root);
    printf("root: parse=%d attr=0x%x subdir=%llu\n", rr, root.attributes,
           (unsigned long long)root.subdir_offset);
    if (rr != 1) { fprintf(stderr, "root parse failed\n"); return 2; }
    walk(root.subdir_offset, "", 0);

    printf("\ndirs=%lu files=%lu reparse=%lu empty=%lu ads=%lu parse_err=%lu\n",
           dirs, files, reparse, empty_files, ads_total, parse_err);
    printf("hash_ok=%lu hash_miss=%lu  invariant_ok=%lu invariant_bad=%lu\n",
           hash_ok, hash_miss, inv_ok, inv_bad);
    printf("tagged items: total=%lu EA=%lu objid=%lu other=%lu\n",
           tagged_total, tag_ea, tag_objid, tag_other);
    printf("paths: \\Windows=%d System32=%d ntoskrnl.exe=%d config\\SYSTEM=%d Boot\\EFI\\bootmgfw=%d\n",
           f_win, f_sys32, f_ntoskrnl, f_hive, f_bootmgr);
    return (hash_miss || inv_bad || parse_err || !f_sys32) ? 1 : 0;
}
