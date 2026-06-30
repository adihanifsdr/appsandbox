/*
 * test_ntfs.c -- build a small NTFS volume, then INDEPENDENTLY verify it by
 * re-reading via raw pread (not the writer's helpers): $Boot, $MFT self-map,
 * USA fixups, root $INDEX_ROOT collation, $FILE_NAME round-trip, resident +
 * non-resident $DATA readback, and $Secure security_id resolution.
 */
#include "ntfs.h"
#include "blockio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* ---------------- independent reader ---------------- */
static int FD;
static uint64_t PART_OFF, MFT_LCN;
static uint32_t CLU=4096;
static int fails=0, checks=0;
#define CHECK(c,msg) do{ checks++; if(!(c)){ fails++; printf("  FAIL: %s\n", msg);} }while(0)

static uint16_t g16(const uint8_t*p){return p[0]|(p[1]<<8);}
static uint32_t g32(const uint8_t*p){return p[0]|(p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t g64(const uint8_t*p){uint64_t v=0;for(int i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}

static void prd(void*b,uint64_t off,size_t n){ if(pread(FD,b,n,(off_t)(PART_OFF+off))!=(ssize_t)n){printf("  pread fail @%llu\n",(unsigned long long)off);} }

/* verify+strip USA fixup in a record of `size` bytes (512-byte sectors) */
static int fixup_strip(uint8_t*rec,uint32_t size){
    uint16_t uo=g16(rec+4), uc=g16(rec+6), usn=g16(rec+uo);
    for(uint32_t i=0;i+1<uc;i++){
        uint8_t*q=rec+(i+1)*512-2;
        if(g16(q)!=usn) return -1;             /* torn record */
        q[0]=rec[uo+2+2*i]; q[1]=rec[uo+2+2*i+1];
    }
    (void)size; return 0;
}
static int read_rec(uint32_t recno, uint8_t out[1024]){
    prd(out, MFT_LCN*CLU + (uint64_t)recno*1024, 1024);
    if(memcmp(out,"FILE",4)) return -1;
    return fixup_strip(out,1024);
}
/* find attribute by type (+optional unicode name); returns pointer or NULL */
static uint8_t* find_attr(uint8_t*rec, uint32_t type, const char*name){
    uint8_t*p=rec+g16(rec+0x14);
    while(g32(p)!=0xFFFFFFFF){
        uint32_t t=g32(p), len=g32(p+4);
        if(t==type){
            int nl=p[9];
            if(!name && nl==0) return p;
            if(name){ int ok=(int)strlen(name)==nl; uint8_t*nm=p+g16(p+0x0A);
                for(int i=0;ok&&i<nl;i++) ok = (g16(nm+2*i)==(uint8_t)name[i]);
                if(ok) return p; }
        }
        p+=len; if(len==0)break;
    }
    return NULL;
}
/* decode a single-run non-resident runlist -> first_lcn,count */
static void decode_run(uint8_t*attr, uint64_t*lcn, uint64_t*count){
    uint8_t*mp=attr+g16(attr+0x20); uint8_t h=mp[0]; int lb=h&0xf, ob=h>>4;
    uint64_t cnt=0; for(int i=0;i<lb;i++) cnt|=(uint64_t)mp[1+i]<<(8*i);
    int64_t off=0; for(int i=0;i<ob;i++) off|=(int64_t)mp[1+lb+i]<<(8*i);
    if(ob && (mp[1+lb+ob-1]&0x80)) off |= -(int64_t)1<<(8*ob);
    *lcn=(uint64_t)off; *count=cnt;
}

/* simple uppercase matching the writer's table for name compare */
static uint16_t up(uint16_t c){ if(c>='a'&&c<='z')return c-32; if(c>=0xE0&&c<=0xFE&&c!=0xF7)return c-32; return c; }
static int name_eq(const uint8_t*nm16,int n,const char*ascii){
    if((int)strlen(ascii)!=n) return 0;
    for(int i=0;i<n;i++) if(up(g16(nm16+2*i))!=up((uint8_t)ascii[i])) return 0;
    return 1;
}

/* find a child entry in a dir record's $INDEX_ROOT by name; returns file_ref or 0 */
static uint64_t index_find(uint8_t*dirrec, const char*name, uint64_t*prev_key_for_sort, int*sorted){
    uint8_t*ir=find_attr(dirrec,0x90,"$I30"); if(!ir) return 0;
    uint8_t*val=ir+g16(ir+0x14);
    uint8_t*ih=val+0x10; uint32_t eo=g32(ih+0x00);
    uint8_t*e=ih+eo; uint64_t found=0;
    while(1){
        uint16_t flags=g16(e+0x0C), elen=g16(e+0x08), klen=g16(e+0x0A);
        if(flags&0x02) break;                  /* END */
        uint64_t ref=g64(e+0x00);
        uint8_t*key=e+0x10; int nl=key[0x40];
        if(name && name_eq(key+0x42,nl,name)) found=ref;
        (void)klen;(void)prev_key_for_sort;(void)sorted;
        e+=elen; if(elen==0)break;
    }
    return found;
}

static void verify(const char*path){
    FD=open(path,O_RDONLY); if(FD<0){printf("open fail\n");fails++;return;}
    uint8_t boot[512]; prd(boot,0,512);
    CHECK(memcmp(boot+3,"NTFS    ",8)==0,"$Boot OEM != 'NTFS    '");
    CHECK(g16(boot+0x1FE)==0xAA55,"$Boot end marker");
    uint32_t spc=boot[0x0D]; CLU=spc*512;
    MFT_LCN=g64(boot+0x30);
    uint64_t mftmirr=g64(boot+0x38);
    printf("  $Boot: spc=%u cluster=%u mft_lcn=%llu mftmirr_lcn=%llu total_sectors=%llu\n",
        spc,CLU,(unsigned long long)MFT_LCN,(unsigned long long)mftmirr,(unsigned long long)g64(boot+0x28));

    /* MFT record 0 self-map */
    uint8_t r0[1024];
    CHECK(read_rec(0,r0)==0,"record 0 fixup/magic");
    uint8_t*data0=find_attr(r0,0x80,NULL); CHECK(data0!=NULL,"$MFT has $DATA");
    if(data0){ uint64_t lcn,cnt; decode_run(data0,&lcn,&cnt);
        CHECK(lcn==MFT_LCN,"$MFT $DATA first lcn == BPB mft_lcn");
        printf("  $MFT $DATA run: lcn=%llu count=%llu\n",(unsigned long long)lcn,(unsigned long long)cnt); }

    /* root dir record 5: enumerate + validate each child's $FILE_NAME round-trips */
    uint8_t r5[1024]; CHECK(read_rec(5,r5)==0,"root record fixup/magic");
    uint8_t*ir=find_attr(r5,0x90,"$I30"); CHECK(ir!=NULL,"root has $INDEX_ROOT");
    if(ir){
        uint8_t*ih=ir+g16(ir+0x14)+0x10; uint8_t*e=ih+g32(ih+0x00);
        int count=0; uint8_t prevkey[600]; int haveprev=0;
        printf("  root entries:");
        while(1){
            uint16_t flags=g16(e+0x0C), elen=g16(e+0x08);
            if(flags&0x02) break;
            uint64_t ref=g64(e+0x00); uint8_t*key=e+0x10; int nl=key[0x40];
            char nm[260]; for(int i=0;i<nl&&i<259;i++) nm[i]=(char)g16(key+0x42+2*i); nm[nl<259?nl:259]=0;
            printf(" '%s'",nm);
            /* collation must be ascending */
            if(haveprev){
                int pn=prevkey[0x40], n=nl<pn?nl:pn, c=0;
                for(int i=0;i<n;i++){uint16_t a=up(g16(prevkey+0x42+2*i)),b=up(g16(key+0x42+2*i)); if(a!=b){c=a<b?-1:1;break;}}
                if(c==0) c = nl<pn?-1:(nl>pn?1:0);
                CHECK(c<0,"root index ascending collation");
            }
            memcpy(prevkey,key,0x42+2*nl); haveprev=1;
            /* round-trip: target record's $FILE_NAME name == key name */
            uint8_t rt[1024];
            if(read_rec((uint32_t)(ref&0xFFFFFFFFFFFF),rt)==0){
                uint8_t*fn=find_attr(rt,0x30,NULL);
                if(fn){ uint8_t*body=fn+g16(fn+0x14); int bnl=body[0x40];
                    CHECK(bnl==nl && memcmp(body+0x42,key+0x42,2*nl)==0,"child $FILE_NAME matches index key"); }
            } else CHECK(0,"child record unreadable");
            count++; e+=elen; if(elen==0)break;
        }
        printf("\n  root child count=%d\n",count);
    }

    /* resolve a known resident file 'hello.txt' and compare $DATA */
    uint64_t href=index_find(r5,"hello.txt",NULL,NULL);
    CHECK(href!=0,"found hello.txt");
    if(href){ uint8_t rt[1024]; read_rec((uint32_t)(href&0xFFFFFFFFFFFF),rt);
        uint8_t*d=find_attr(rt,0x80,NULL); CHECK(d && d[8]==0,"hello.txt $DATA resident");
        if(d&&d[8]==0){ uint32_t vl=g32(d+0x10); uint8_t*v=d+g16(d+0x14);
            CHECK(vl==13 && memcmp(v,"hello, world!",13)==0,"hello.txt content"); }
        uint8_t*si=find_attr(rt,0x10,NULL);
        CHECK(si && g32(si+g16(si+0x14)+0x34)==0x100,"hello.txt security_id==0x100"); }

    /* resolve a non-resident file and compare data from its run */
    uint64_t bref=index_find(r5,"BIGFILE.BIN",NULL,NULL);
    CHECK(bref!=0,"found BIGFILE.BIN");
    if(bref){ uint8_t rt[1024]; read_rec((uint32_t)(bref&0xFFFFFFFFFFFF),rt);
        uint8_t*d=find_attr(rt,0x80,NULL); CHECK(d && d[8]==1,"BIGFILE non-resident");
        if(d&&d[8]==1){ uint64_t lcn,cnt; decode_run(d,&lcn,&cnt); uint64_t dsz=g64(d+0x30);
            uint8_t*buf=malloc(dsz); prd(buf, lcn*CLU, dsz);
            int ok=1; for(uint64_t i=0;i<dsz;i++) if(buf[i]!=(uint8_t)(i*7+1)){ok=0;break;}
            CHECK(ok,"BIGFILE.BIN content via run"); free(buf); } }

    /* subdir round-trip: find subdir, then a child inside it */
    uint64_t sref=index_find(r5,"subdir",NULL,NULL);
    CHECK(sref!=0,"found subdir");
    if(sref){ uint8_t sd[1024]; read_rec((uint32_t)(sref&0xFFFFFFFFFFFF),sd);
        CHECK((g16(sd+0x16)&0x02)!=0,"subdir record DIRECTORY flag");
        uint64_t cref=index_find(sd,"inner.dat",NULL,NULL);
        CHECK(cref!=0,"found subdir/inner.dat"); }

    /* $Secure $SII contains id 0x100 */
    uint8_t r9[1024]; CHECK(read_rec(9,r9)==0,"$Secure record");
    uint8_t*sii=find_attr(r9,0x90,"$SII"); CHECK(sii!=NULL,"$Secure has $SII");
    if(sii){ uint8_t*ih=sii+g16(sii+0x14)+0x10; uint8_t*e=ih+g32(ih+0x00); int found=0;
        while(1){ uint16_t flags=g16(e+0x0C),elen=g16(e+0x08); if(flags&0x02)break;
            uint16_t doff=g16(e+0x00); uint32_t id=g32(e+0x10); (void)doff;
            if(id==0x100) found=1; e+=elen; if(elen==0)break; }
        CHECK(found,"$SII has security_id 0x100"); }

    close(FD);
}

/* ---------------- builder ---------------- */
static int u16s(uint16_t*o,const char*s){ int n=0; while(*s) o[n++]=(uint8_t)*s++; return n; }

int main(int argc,char**argv){
    setvbuf(stdout,NULL,_IONBF,0);
    const char*path = argc>1?argv[1]:"/tmp/test_ntfs.img";
    uint64_t part_lba = argc>2?strtoull(argv[2],0,10):0;   /* NTFS partition start LBA */
    uint64_t bytes = 96ull<<20;          /* 96 MiB volume */
    uint64_t total = part_lba*512 + bytes;
    blockio_t*io=blockio_create(path,total);
    if(!io){printf("blockio_create fail\n");return 2;}
    PART_OFF = part_lba*512;

    ntfs_writer_t*w=ntfs_writer_open(io,part_lba,bytes/512,"TESTVOL");
    if(!w){printf("ntfs_writer_open fail\n");return 2;}
    uint64_t root=ntfs_root_ref(w);
    int32_t sid=ntfs_secure_intern(w,NULL,0);
    uint64_t T=0x01da84c3f3b79090ull;
    uint16_t nm[64]; int n;

    n=u16s(nm,"hello.txt");
    ntfs_add_file(w,root,nm,n,NULL,0,0x20,sid,T,T,T,"hello, world!",13);

    static uint8_t big[10000]; for(int i=0;i<10000;i++) big[i]=(uint8_t)(i*7+1);
    n=u16s(nm,"BIGFILE.BIN");
    ntfs_add_file(w,root,nm,n,NULL,0,0x20,sid,T,T,T,big,sizeof big);

    n=u16s(nm,"subdir");
    uint64_t sub=ntfs_add_dir(w,root,nm,n,NULL,0,0x10,sid,T,T,T);
    n=u16s(nm,"inner.dat");
    ntfs_add_file(w,sub,nm,n,NULL,0,0x20,sid,T,T,T,"inside",6);

    n=u16s(nm,"readme.md");
    ntfs_add_file(w,root,nm,n,NULL,0,0x20,sid,T,T,T,"# readme",8);

    int rc=ntfs_writer_close(w);
    printf("ntfs_writer_close rc=%d\n",rc);
    blockio_close(io);
    if(rc){printf("BUILD FAILED\n");return 1;}

    printf("--- verify ---\n");
    verify(path);
    printf("--- %d checks, %d failures ---\n", checks, fails);
    return fails?1:0;
}
