/*
 * test_btree.c -- exercise the $INDEX_ALLOCATION B-tree: build a directory with
 * thousands of entries (overflowing $INDEX_ROOT) and INDEPENDENTLY verify the
 * tree by in-order traversal: collect every leaf entry, assert the full set is
 * sorted, complete, and each entry's $FILE_NAME round-trips to its record.
 */
#include "ntfs.h"
#include "blockio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static int FD; static uint64_t PART_OFF, MFT_LCN; static uint32_t CLU=4096;
static int fails=0, checks=0;
#define CHECK(c,msg) do{ checks++; if(!(c)){ fails++; printf("  FAIL: %s\n", msg);} }while(0)
static uint16_t g16(const uint8_t*p){return p[0]|(p[1]<<8);}
static uint32_t g32(const uint8_t*p){return p[0]|(p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t g64(const uint8_t*p){uint64_t v=0;for(int i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static void prd(void*b,uint64_t off,size_t n){ pread(FD,b,n,(off_t)(PART_OFF+off)); }
static int fixup_strip(uint8_t*rec,uint32_t size){
    uint16_t uo=g16(rec+4),uc=g16(rec+6),usn=g16(rec+uo);
    for(uint32_t i=0;i+1<uc;i++){ uint8_t*q=rec+(i+1)*512-2; if(g16(q)!=usn)return -1; q[0]=rec[uo+2+2*i];q[1]=rec[uo+2+2*i+1]; }
    (void)size; return 0;
}
static int read_rec(uint32_t recno,uint8_t out[1024]){ prd(out,MFT_LCN*CLU+(uint64_t)recno*1024,1024); if(memcmp(out,"FILE",4))return -1; return fixup_strip(out,1024); }
static uint8_t* find_attr(uint8_t*rec,uint32_t type,const char*name){
    uint8_t*p=rec+g16(rec+0x14);
    while(g32(p)!=0xFFFFFFFF){ uint32_t t=g32(p),len=g32(p+4);
        if(t==type){ int nl=p[9];
            if(!name&&nl==0)return p;
            if(name){int ok=(int)strlen(name)==nl; uint8_t*nm=p+g16(p+0x0A); for(int i=0;ok&&i<nl;i++)ok=(g16(nm+2*i)==(uint8_t)name[i]); if(ok)return p; } }
        p+=len; if(!len)break; }
    return NULL;
}
static void decode_run(uint8_t*attr,uint64_t*lcn,uint64_t*count){
    uint8_t*mp=attr+g16(attr+0x20); uint8_t h=mp[0]; int lb=h&0xf,ob=h>>4; uint64_t c=0; for(int i=0;i<lb;i++)c|=(uint64_t)mp[1+i]<<(8*i);
    int64_t o=0; for(int i=0;i<ob;i++)o|=(int64_t)mp[1+lb+i]<<(8*i); if(ob&&(mp[1+lb+ob-1]&0x80))o|=-(int64_t)1<<(8*ob); *lcn=(uint64_t)o; *count=c;
}
static uint16_t up(uint16_t c){ if(c>='a'&&c<='z')return c-32; if(c>=0xE0&&c<=0xFE&&c!=0xF7)return c-32; return c; }

/* collected entries (in traversal order) */
static char (*NAMES)[64]; static uint64_t *REFS; static uint32_t NCOLL, CAPCOLL;
static void collect(const uint8_t*key, uint64_t ref){
    if(NCOLL==CAPCOLL){ CAPCOLL=CAPCOLL?CAPCOLL*2:1024; NAMES=realloc(NAMES,CAPCOLL*sizeof(*NAMES)); REFS=realloc(REFS,CAPCOLL*sizeof(*REFS)); }
    int nl=key[0x40]; int j=0; for(int i=0;i<nl&&j<63;i++) NAMES[NCOLL][j++]=(char)g16(key+0x42+2*i); NAMES[NCOLL][j]=0; REFS[NCOLL]=ref; NCOLL++;
}
/* in-order walk of a node: `ih` points at the INDEX_HEADER; entries at ih+entries_offset.
 * idx_lcn/blocks describe the $INDEX_ALLOCATION region for descending into children. */
static void walk_node(uint8_t*ih, uint64_t idx_lcn){
    uint8_t*e=ih+g32(ih+0x00);
    for(;;){
        uint16_t flags=g16(e+0x0C), elen=g16(e+0x08);
        if(flags&0x01){ /* HAS_SUBNODE: descend left child first */
            uint64_t vcn=g64(e+elen-8);
            uint8_t*blk=malloc(4096); prd(blk, idx_lcn*CLU+vcn*4096, 4096);
            if(!memcmp(blk,"INDX",4) && fixup_strip(blk,4096)==0) walk_node(blk+0x18, idx_lcn);
            else { checks++; fails++; printf("  FAIL: bad INDX vcn=%llu\n",(unsigned long long)vcn); }
            free(blk);
        }
        if(flags&0x02) break;            /* END */
        collect(e+0x10, g64(e+0x00));
        e+=elen; if(!elen)break;
    }
}

static void verify(const char*path, uint32_t expect_n){
    FD=open(path,O_RDONLY); if(FD<0){printf("open fail\n");fails++;return;}
    uint8_t boot[512]; prd(boot,0,512); CLU=boot[0x0D]*512; MFT_LCN=g64(boot+0x30);
    /* find "huge" in root */
    uint8_t r5[1024]; read_rec(5,r5);
    uint8_t*ir=find_attr(r5,0x90,"$I30"); uint8_t*ih=ir+g16(ir+0x14)+0x10; uint8_t*e=ih+g32(ih+0x00);
    uint64_t huge=0;
    while(1){ uint16_t fl=g16(e+0x0C),el=g16(e+0x08); if(fl&0x02)break; uint8_t*k=e+0x10; int nl=k[0x40];
        char nm[64]; for(int i=0;i<nl;i++)nm[i]=(char)g16(k+0x42+2*i); nm[nl]=0; if(!strcmp(nm,"huge"))huge=g64(e+0x00); e+=el; if(!el)break; }
    CHECK(huge!=0,"found 'huge' dir");
    if(!huge){ close(FD); return; }

    uint8_t hr[1024]; read_rec((uint32_t)(huge&0xFFFFFFFFFFFF),hr);
    uint8_t*hir=find_attr(hr,0x90,"$I30"); CHECK(hir!=NULL,"huge has $INDEX_ROOT");
    uint8_t*hih=hir+g16(hir+0x14)+0x10;
    CHECK((hih[0x0C]&0x01)!=0,"huge $INDEX_ROOT is LARGE");
    uint8_t*ia=find_attr(hr,0xA0,"$I30"); CHECK(ia!=NULL,"huge has $INDEX_ALLOCATION");
    uint8_t*bm=find_attr(hr,0xB0,"$I30"); CHECK(bm!=NULL,"huge has $BITMAP");
    uint64_t idx_lcn=0,idx_blocks=0; if(ia) decode_run(ia,&idx_lcn,&idx_blocks);
    printf("  huge: idx_lcn=%llu blocks=%llu\n",(unsigned long long)idx_lcn,(unsigned long long)idx_blocks);

    NCOLL=0;
    walk_node(hih, idx_lcn);
    printf("  collected %u entries (expected %u)\n", NCOLL, expect_n);
    CHECK(NCOLL==expect_n,"entry count matches");

    /* sorted ascending + each ref round-trips */
    int sorted=1, rt_ok=1;
    for(uint32_t i=0;i<NCOLL;i++){
        if(i){ const char*a=NAMES[i-1],*b=NAMES[i]; int c=0; for(int k=0;a[k]||b[k];k++){uint16_t x=up((uint8_t)a[k]),y=up((uint8_t)b[k]); if(x!=y){c=x<y?-1:1;break;} if(!a[k]||!b[k])break;} if(c>=0)sorted=0; }
        uint8_t rt[1024]; if(read_rec((uint32_t)(REFS[i]&0xFFFFFFFFFFFF),rt)==0){ uint8_t*fn=find_attr(rt,0x30,NULL);
            if(fn){ uint8_t*body=fn+g16(fn+0x14); int bnl=body[0x40]; char rn[64]; for(int k=0;k<bnl;k++)rn[k]=(char)g16(body+0x42+2*k); rn[bnl]=0; if(strcmp(rn,NAMES[i]))rt_ok=0; } else rt_ok=0; }
        else rt_ok=0;
    }
    CHECK(sorted,"all entries sorted ascending");
    CHECK(rt_ok,"every entry $FILE_NAME round-trips");
    close(FD);
}

static int u16s(uint16_t*o,const char*s){int n=0;while(*s)o[n++]=(uint8_t)*s++;return n;}
int main(int argc,char**argv){
    setvbuf(stdout,NULL,_IONBF,0);
    const char*path=argc>1?argv[1]:"/tmp/test_btree.img";
    uint32_t N=argc>2?(uint32_t)atoi(argv[2]):3000;
    uint64_t bytes=256ull<<20;
    blockio_t*io=blockio_create(path,bytes);
    ntfs_writer_t*w=ntfs_writer_open(io,0,bytes/512,"BTREE");
    uint64_t root=ntfs_root_ref(w); int32_t sid=ntfs_secure_intern(w,NULL,0);
    uint64_t T=0x01da84c3f3b79090ull; uint16_t nm[64]; int n;
    n=u16s(nm,"huge"); uint64_t huge=ntfs_add_dir(w,root,nm,n,NULL,0,0x10,sid,T,T,T);
    for(uint32_t i=0;i<N;i++){ char b[32]; snprintf(b,sizeof b,"f%05u.dat",i); n=u16s(nm,b);
        char content[16]; int cl=snprintf(content,sizeof content,"#%u",i);
        ntfs_add_file(w,huge,nm,n,NULL,0,0x20,sid,T,T,T,content,cl); }
    int rc=ntfs_writer_close(w); printf("close rc=%d\n",rc);
    blockio_close(io);
    if(rc){printf("BUILD FAILED\n");return 1;}
    printf("--- verify (N=%u) ---\n",N);
    verify(path,N);
    printf("--- %d checks, %d failures ---\n",checks,fails);
    return fails?1:0;
}
