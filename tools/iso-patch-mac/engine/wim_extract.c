/*
 * wim_extract.c -- resolve a backslash path inside a WIM image and either list a
 * directory or extract a file (decompressed). Scouting tool for what the disk
 * builder must source from the provided ISO.
 *   wim_extract <install.wim> <image_idx> <\path> [out_file]
 */
#include "wimimg.h"
#include "wim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static wim_t *W; static wim_image_t *IMG;

static int name_eq(const uint8_t *nm16, int nb, const char *ascii){
    int n = nb/2; if ((int)strlen(ascii)!=n) return 0;
    for (int i=0;i<n;i++){
        unsigned ch = nm16[2*i] | (nm16[2*i+1]<<8);
        char a = ascii[i];
        if (ch>='a'&&ch<='z') ch-=32; if (a>='a'&&a<='z') a-=32;
        if (ch != (unsigned)a) return 0;
    }
    return 1;
}
/* find child `comp` under children list at off; fills d; returns 1/0 */
static int find_child(uint64_t off, const char *comp, int clen, wim_dentry_t *out){
    char c[256]; if (clen>255) clen=255; memcpy(c,comp,clen); c[clen]=0;
    for (;;){
        wim_dentry_t d; int r = wim_dentry_at(IMG, off, &d);
        if (r<=0) break;
        if (name_eq(d.file_name, d.file_name_nbytes, c)){ *out=d; return 1; }
        off = d.self_offset + d.length;
    }
    return 0;
}
static int resolve(const char *path, wim_dentry_t *out){
    wim_dentry_t root; wim_dentry_at(IMG, wim_image_root_offset(IMG), &root);
    uint64_t children = root.subdir_offset;
    const char *p=path; wim_dentry_t cur; int have=0;
    while (*p){
        while (*p=='\\'||*p=='/') p++;
        if (!*p) break;
        const char *comp=p; int clen=0; while (*p&&*p!='\\'&&*p!='/'){ p++; clen++; }
        if (!find_child(children, comp, clen, &cur)) return 0;
        have=1; children = cur.subdir_offset;
    }
    if (!have) return 0;
    *out=cur; return 1;
}

int main(int argc,char**argv){
    if (argc<4){ printf("usage: wim_extract <wim> <idx> <\\path> [out]\n"); return 2; }
    W=wim_open(argv[1]); IMG=wim_image_open(W,(uint32_t)atoi(argv[2]));
    if (!W||!IMG){ printf("open fail\n"); return 2; }
    wim_dentry_t d;
    if (!resolve(argv[3], &d)){ printf("NOT FOUND: %s\n", argv[3]); return 1; }

    if (d.attributes & WIM_FILE_ATTRIBUTE_DIRECTORY){
        printf("DIR %s:\n", argv[3]);
        uint64_t off=d.subdir_offset; int n=0;
        for(;;){ wim_dentry_t c; int r=wim_dentry_at(IMG,off,&c); if(r<=0)break;
            int nc=c.file_name_nbytes/2; char nm[260]; for(int i=0;i<nc&&i<259;i++) nm[i]=(char)(c.file_name[2*i]|(c.file_name[2*i+1]<<8)); nm[nc<259?nc:259]=0;
            int empty=1; for(int i=0;i<20;i++) if(c.hash[i]){empty=0;break;}
            const wim_resource_t*res = empty?NULL:wim_lookup_by_hash(W,c.hash);
            printf("  %-50s %s  %llu bytes\n", nm, (c.attributes&WIM_FILE_ATTRIBUTE_DIRECTORY)?"<DIR>":"file",
                   res?(unsigned long long)res->orig_size:0ull);
            off=c.self_offset+c.length; n++; }
        printf("  (%d entries)\n", n);
    } else {
        const wim_resource_t *res = wim_lookup_by_hash(W, d.hash);
        if (!res){ printf("empty/unresolved file\n"); return 1; }
        printf("FILE %s  size=%llu  reparse_tag=0x%x\n", argv[3], (unsigned long long)res->orig_size, d.reparse_tag);
        if (argc>4){
            uint8_t *buf=malloc((size_t)res->orig_size);
            if (wim_read_resource(W,res,buf)!=0){ printf("decode fail\n"); return 1; }
            FILE*f=fopen(argv[4],"wb"); fwrite(buf,1,(size_t)res->orig_size,f); fclose(f);
            printf("wrote %llu bytes to %s\n",(unsigned long long)res->orig_size,argv[4]);
            free(buf);
        }
    }
    return 0;
}
