/*
 * win2ntfs.c -- integrate the WIM reader and NTFS writer: apply a full image
 * from install.wim into an NTFS partition (the win_disk.c data-flow core).
 * Then verify end-to-end: path-resolve known files through the on-disk $I30
 * index (B-tree aware), read their $DATA back off NTFS, SHA-1 it, and require a
 * match with the WIM lookup-table hash.
 *
 *   win2ntfs <install.wim> <out.img> [image_index]
 */
#include "wimimg.h"
#include "wim.h"
#include "ntfs.h"
#include "blockio.h"
#include "sha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static wim_t *W; static wim_image_t *IMG; static ntfs_writer_t *NW;
static int32_t *SIDMAP;            /* wim security_id -> ntfs security_id (cached) */
static unsigned long g_dirs, g_files, g_empty, g_reparse, g_err;
static unsigned long long g_bytes;

static int32_t map_sid(int32_t wim_sid){
    /* All files share the default descriptor at security_id 0x100 (boots per
     * NTFS-WIM-DESIGN §5). Per-file WIM SDs would need a $Secure B-tree. */
    (void)wim_sid;
    return ntfs_secure_intern(NW, NULL, 0);
}

static void walk(uint64_t parent_ref, uint64_t children_off, int depth){
    if (depth > 96) return;
    uint64_t off = children_off;
    for (;;){
        wim_dentry_t d;
        int r = wim_dentry_at(IMG, off, &d);
        if (r == 0) break;
        if (r < 0){ g_err++; break; }

        uint16_t nm[256]; int nc = d.file_name_nbytes/2; if (nc>255) nc=255;
        for (int i=0;i<nc;i++) nm[i] = (uint16_t)(d.file_name[2*i] | (d.file_name[2*i+1]<<8));
        int32_t sid = map_sid(d.security_id);

        if (d.attributes & WIM_FILE_ATTRIBUTE_DIRECTORY){
            uint64_t ref = ntfs_add_dir(NW, parent_ref, nm, nc, NULL, 0, d.attributes, sid,
                                        d.creation_time, d.last_access_time, d.last_write_time);
            if (!ref){ g_err++; }
            else { g_dirs++; if (d.subdir_offset) walk(ref, d.subdir_offset, depth+1); }
        } else if (d.attributes & WIM_FILE_ATTRIBUTE_REPARSE_POINT){
            /* create as an empty file; the reparse buffer is not emitted */
            ntfs_add_file(NW, parent_ref, nm, nc, NULL, 0, d.attributes & ~WIM_FILE_ATTRIBUTE_REPARSE_POINT,
                          sid, d.creation_time, d.last_access_time, d.last_write_time, NULL, 0);
            g_reparse++;
        } else {
            int empty = 1; for (int i=0;i<20;i++) if (d.hash[i]) { empty=0; break; }
            if (empty){
                ntfs_add_file(NW, parent_ref, nm, nc, NULL, 0, d.attributes, sid,
                              d.creation_time, d.last_access_time, d.last_write_time, NULL, 0);
                g_empty++;
            } else {
                const wim_resource_t *res = wim_lookup_by_hash(W, d.hash);
                if (!res){ g_err++; off = d.self_offset + d.length; continue; }
                uint8_t *buf = malloc((size_t)res->orig_size);
                if (wim_read_resource(W, res, buf) != 0){ g_err++; free(buf); off=d.self_offset+d.length; continue; }
                ntfs_add_file(NW, parent_ref, nm, nc, NULL, 0, d.attributes, sid,
                              d.creation_time, d.last_access_time, d.last_write_time, buf, res->orig_size);
                g_bytes += res->orig_size;
                free(buf);
                g_files++;
            }
        }
        if ((g_files + g_dirs) % 20000 == 0)
            printf("  ... %lu dirs, %lu files, %.1f GiB\n", g_dirs, g_files, g_bytes/1073741824.0);
        off = d.self_offset + d.length;
    }
}

/* ---------------- end-to-end verifier (raw pread + B-tree path resolve) ----- */
static int FD; static uint64_t MFT_LCN; static uint32_t CLU=4096;
static uint16_t g16(const uint8_t*p){return p[0]|(p[1]<<8);}
static uint32_t g32(const uint8_t*p){return p[0]|(p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t g64(const uint8_t*p){uint64_t v=0;for(int i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static int fixup_strip(uint8_t*rec){ uint16_t uo=g16(rec+4),uc=g16(rec+6),usn=g16(rec+uo);
    for(uint32_t i=0;i+1<uc;i++){uint8_t*q=rec+(i+1)*512-2; if(g16(q)!=usn)return -1; q[0]=rec[uo+2+2*i];q[1]=rec[uo+2+2*i+1];} return 0; }
static int read_rec(uint32_t n,uint8_t out[1024]){ pread(FD,out,1024,(off_t)(MFT_LCN*CLU+(uint64_t)n*1024)); if(memcmp(out,"FILE",4))return -1; return fixup_strip(out); }
static uint8_t* find_attr(uint8_t*rec,uint32_t type,const char*name){
    uint8_t*p=rec+g16(rec+0x14);
    while(g32(p)!=0xFFFFFFFF){ uint32_t t=g32(p),len=g32(p+4);
        if(t==type){ int nl=p[9]; if(!name&&nl==0)return p;
            if(name){int ok=(int)strlen(name)==nl;uint8_t*z=p+g16(p+0x0A);for(int i=0;ok&&i<nl;i++)ok=(g16(z+2*i)==(uint8_t)name[i]);if(ok)return p;} }
        p+=len; if(!len)break; }
    return NULL;
}
static uint16_t up(uint16_t c){ if(c>='a'&&c<='z')return c-32; if(c>=0xE0&&c<=0xFE&&c!=0xF7)return c-32; return c; }
static int cmp_key(const uint8_t*key,const char*name){ /* returns sign(name - key) by collation */
    int kn=key[0x40], tn=(int)strlen(name), n=kn<tn?kn:tn;
    for(int i=0;i<n;i++){ uint16_t k=up(g16(key+0x42+2*i)), t=up((uint8_t)name[i]); if(t!=k) return t<k?-1:1; }
    return tn==kn?0:(tn<kn?-1:1);
}
static uint64_t search_node(uint8_t*ih, uint64_t idx_lcn, const char*name){
    uint8_t*e=ih+g32(ih+0x00);
    for(;;){
        uint16_t fl=g16(e+0x0C), el=g16(e+0x08);
        if(!(fl&0x02)){
            int c=cmp_key(e+0x10,name);
            if(c==0) return g64(e+0x00);
            if(c<0){ if(fl&0x01){ uint64_t v=g64(e+el-8); uint8_t*b=malloc(4096); pread(FD,b,4096,(off_t)(idx_lcn*CLU+v*4096));
                        uint64_t r=(!memcmp(b,"INDX",4)&&fixup_strip(b)==0)?search_node(b+0x18,idx_lcn,name):0; free(b); return r; } return 0; }
            e+=el; if(!el)return 0; continue;
        } else { if(fl&0x01){ uint64_t v=g64(e+el-8); uint8_t*b=malloc(4096); pread(FD,b,4096,(off_t)(idx_lcn*CLU+v*4096));
                    uint64_t r=(!memcmp(b,"INDX",4)&&fixup_strip(b)==0)?search_node(b+0x18,idx_lcn,name):0; free(b); return r; } return 0; }
    }
}
static uint64_t dir_lookup(uint8_t*dirrec,const char*name){
    uint8_t*ir=find_attr(dirrec,0x90,"$I30"); if(!ir)return 0;
    uint8_t*ia=find_attr(dirrec,0xA0,"$I30"); uint64_t idx_lcn=0,blk=0;
    if(ia){ uint8_t*mp=ia+g16(ia+0x20); int lb=mp[0]&0xf,ob=mp[0]>>4; for(int i=0;i<lb;i++)blk|=(uint64_t)mp[1+i]<<(8*i);
            int64_t o=0;for(int i=0;i<ob;i++)o|=(int64_t)mp[1+lb+i]<<(8*i); idx_lcn=(uint64_t)o; (void)blk; }
    return search_node(ir+g16(ir+0x14)+0x10, idx_lcn, name);
}
/* resolve a backslash path from root; returns ref or 0 */
static uint64_t resolve(const char*path){
    uint8_t rec[1024]; if(read_rec(5,rec))return 0; uint64_t ref=((uint64_t)5)|((uint64_t)1<<48);
    char comp[260]; const char*p=path;
    while(*p){ while(*p=='\\')p++; if(!*p)break; int n=0; while(*p&&*p!='\\'&&n<259)comp[n++]=*p++; comp[n]=0;
        ref=dir_lookup(rec,comp); if(!ref)return 0; if(read_rec((uint32_t)(ref&0xFFFFFFFFFFFF),rec))return 0; }
    return ref;
}
/* read a file's $DATA (resident or single-run non-resident) into malloc'd buf */
static uint8_t* read_data(uint8_t*rec, uint64_t*size_out){
    uint8_t*d=find_attr(rec,0x80,NULL); if(!d)return NULL;
    if(d[8]==0){ uint32_t vl=g32(d+0x10); uint8_t*b=malloc(vl?vl:1); memcpy(b,d+g16(d+0x14),vl); *size_out=vl; return b; }
    uint64_t dsz=g64(d+0x30); uint8_t*mp=d+g16(d+0x20); int lb=mp[0]&0xf,ob=mp[0]>>4; uint64_t cnt=0; for(int i=0;i<lb;i++)cnt|=(uint64_t)mp[1+i]<<(8*i);
    int64_t o=0; for(int i=0;i<ob;i++)o|=(int64_t)mp[1+lb+i]<<(8*i); if(ob&&(mp[1+lb+ob-1]&0x80))o|=-(int64_t)1<<(8*ob);
    uint8_t*b=malloc(dsz?dsz:1); pread(FD,b,dsz,(off_t)((uint64_t)o*CLU)); *size_out=dsz; return b;
}
static void verify_file(const char*path){
    uint64_t ref=resolve(path);
    if(!ref){ printf("  MISS %s (not found)\n",path); g_err++; return; }
    uint8_t rec[1024]; read_rec((uint32_t)(ref&0xFFFFFFFFFFFF),rec);
    uint64_t sz; uint8_t*data=read_data(rec,&sz);
    uint8_t h[20]; sha1(data, sz?sz:0, h);
    /* readback check: bytes read back off NTFS must hash to a known WIM blob */
    const wim_resource_t *res = wim_lookup_by_hash(W, h);
    int ok = res && res->orig_size==sz;
    printf("  %-44s size=%-9llu sha1=%02x%02x%02x%02x  %s\n", path,
           (unsigned long long)sz, h[0],h[1],h[2],h[3], ok?"OK (matches WIM blob)":"FAIL");
    if(!ok) g_err++;
    free(data);
}

int main(int argc,char**argv){
    setvbuf(stdout,NULL,_IONBF,0);
    if(argc<3){ printf("usage: win2ntfs <install.wim> <out.img> [idx]\n"); return 2; }
    const char*wimp=argv[1], *imgp=argv[2]; uint32_t idx=argc>3?(uint32_t)atoi(argv[3]):1;

    W=wim_open(wimp); if(!W){printf("wim_open fail\n");return 2;}
    IMG=wim_image_open(W,idx); if(!IMG){printf("image_open fail\n");return 2;}
    SIDMAP=calloc(wim_image_sd_count(IMG)+1, sizeof(int32_t));

    uint64_t bytes = 32ull<<30;        /* 32 GiB sparse image (data ~24 GiB + cluster slack + metadata) */
    blockio_t*io=blockio_create(imgp,bytes); if(!io){printf("blockio_create fail\n");return 2;}
    NW=ntfs_writer_open(io,0,bytes/512,"Windows");
    uint64_t root=ntfs_root_ref(NW);

    wim_dentry_t r; wim_dentry_at(IMG, wim_image_root_offset(IMG), &r);
    printf("applying image %u ...\n", idx);
    walk(root, r.subdir_offset, 0);
    printf("tree applied: %lu dirs, %lu files, %lu empty, %lu reparse, %lu err, %.2f GiB\n",
           g_dirs,g_files,g_empty,g_reparse,g_err,g_bytes/1073741824.0);

    printf("closing NTFS ...\n");
    int rc=ntfs_writer_close(NW);
    printf("ntfs_writer_close rc=%d\n",rc);
    blockio_close(io);
    if(rc){printf("BUILD FAILED\n");return 1;}

    printf("--- end-to-end verify ---\n");
    FD=open(imgp,O_RDONLY);
    uint8_t boot[512]; pread(FD,boot,512,0); CLU=boot[0x0D]*512; MFT_LCN=g64(boot+0x30);
    g_err=0;
    verify_file("\\Windows\\System32\\ntoskrnl.exe");
    verify_file("\\Windows\\System32\\config\\SYSTEM");
    verify_file("\\Windows\\System32\\drivers\\ntfs.sys");
    verify_file("\\Windows\\Boot\\EFI\\bootmgfw.efi");
    verify_file("\\Windows\\explorer.exe");
    close(FD);
    printf("verify errors: %lu\n", g_err);
    return g_err?1:0;
}
