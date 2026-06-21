/*
 * fat32.c -- minimal FAT32 ESP writer with VFAT long-filename (LFN) support.
 * See fat32.h. Names that aren't clean 8.3 (e.g. "Microsoft", locale dirs,
 * Fonts) get a unique "NAME~n" short alias plus LFN entries, exactly as the
 * UEFI FAT driver expects.
 */
#include "fat32.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void w16(uint8_t *p, uint16_t v){ p[0]=v; p[1]=v>>8; }
static void w32(uint8_t *p, uint32_t v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }

typedef struct fnode {
    char     longname[256];   /* original component */
    int      longlen;
    char     name83[11];      /* space-padded 8.3 (alias if needs_lfn) */
    uint8_t  lcase;           /* lowercase flags (clean-8.3 case only) */
    int      needs_lfn;
    int      is_dir;
    uint32_t first_cluster;
    uint32_t size;
    uint32_t nclusters;
    struct fnode **child; int nchild, cap;
    struct fnode *parent;
} fnode_t;

struct fat32_writer {
    blockio_t *io;
    uint64_t part_off, part_sectors;
    uint32_t sectsz, spc, reserved, num_fats, fat_size, cluster_size;
    uint32_t total_clusters;
    uint32_t *fat; uint32_t fat_cap;
    uint32_t next_free;
    uint32_t serial;
    char label[12];
    fnode_t *root;
};

static uint32_t data_start_sector(fat32_writer_t *w){ return w->reserved + w->num_fats*w->fat_size; }
static uint64_t cluster_byte(fat32_writer_t *w, uint32_t clu){
    return w->part_off + (uint64_t)(data_start_sector(w) + (clu-2)*w->spc) * w->sectsz;
}
static uint32_t alloc_cluster(fat32_writer_t *w){
    uint32_t c = w->next_free++;
    if (c >= w->fat_cap) return 0;
    w->fat[c] = 0x0FFFFFFF;
    return c;
}

/* Try to represent comp as a clean 8.3 (uppercase, or all-lower base/ext via the
 * lcase flags). Returns 0 + fills name83/lcase, or -1 if an LFN is required. */
static int clean_83(const char *comp, int len, char out[11], uint8_t *lcase){
    memset(out,' ',11); *lcase=0;
    if (len<1 || len>12) return -1;
    int dot=-1; for (int i=0;i<len;i++) if (comp[i]=='.'){ if(dot>=0) return -1; dot=i; }
    int bl=(dot<0)?len:dot, el=(dot<0)?0:(len-dot-1);
    if (bl<1 || bl>8 || el>3) return -1;
    if (dot==0) return -1;
    int blo=0,bup=0,elo=0,eup=0;
    for (int i=0;i<bl;i++){ char c=comp[i];
        if (c>='a'&&c<='z'){blo=1; out[i]=c-32;}
        else if (c>='A'&&c<='Z'){bup=1; out[i]=c;}
        else if ((c>='0'&&c<='9')||c=='_'||c=='-'||c=='~'||c=='!'||c=='#'||c=='$') out[i]=c;
        else return -1;
    }
    for (int i=0;i<el;i++){ char c=comp[dot+1+i];
        if (c>='a'&&c<='z'){elo=1; out[8+i]=c-32;}
        else if (c>='A'&&c<='Z'){eup=1; out[8+i]=c;}
        else if ((c>='0'&&c<='9')||c=='_'||c=='-') out[8+i]=c;
        else return -1;
    }
    if (blo&&bup) return -1;        /* mixed case can't be flagged */
    if (elo&&eup) return -1;
    if (blo) *lcase|=0x08;
    if (elo) *lcase|=0x10;
    return 0;
}

static fnode_t *find_child83(fnode_t *d, const char n83[11]){
    for (int i=0;i<d->nchild;i++) if (!memcmp(d->child[i]->name83,n83,11)) return d->child[i];
    return NULL;
}
static int ieq(const char *a,int al,const char *b,int bl){
    if (al!=bl) return 0;
    for (int i=0;i<al;i++){ char x=a[i],y=b[i]; if(x>='a'&&x<='z')x-=32; if(y>='a'&&y<='z')y-=32; if(x!=y)return 0; }
    return 1;
}
static fnode_t *find_child(fnode_t *d, const char *comp, int len){
    for (int i=0;i<d->nchild;i++) if (ieq(d->child[i]->longname,d->child[i]->longlen,comp,len)) return d->child[i];
    return NULL;
}

static void gen_alias(fnode_t *parent, const char *comp, int len, char out[11]){
    int dot=-1; for (int i=len-1;i>=0;i--) if (comp[i]=='.'){ dot=i; break; }
    int baseend=(dot<0)?len:dot;
    char base[8]; int bl=0;
    for (int i=0;i<baseend && bl<6;i++){ char c=comp[i]; if(c==' '||c=='.')continue;
        if(c>='a'&&c<='z')c-=32; if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))c='_'; base[bl++]=c; }
    if (!bl) base[bl++]='_';
    char ext[3]; int el=0;
    if (dot>=0) for (int i=dot+1;i<len && el<3;i++){ char c=comp[i];
        if(c>='a'&&c<='z')c-=32; if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')))c='_'; ext[el++]=c; }
    for (int seq=1; seq<1000; seq++){
        char tmp[11]; memset(tmp,' ',11);
        char suff[6]; int sl=snprintf(suff,sizeof suff,"~%d",seq);
        int keep=bl; if (keep+sl>8) keep=8-sl;
        for (int i=0;i<keep;i++) tmp[i]=base[i];
        for (int i=0;i<sl;i++) tmp[keep+i]=suff[i];
        for (int i=0;i<el;i++) tmp[8+i]=ext[i];
        if (!find_child83(parent,tmp)){ memcpy(out,tmp,11); return; }
    }
    memcpy(out,"ALIAS  ~1",11);
}

static fnode_t *make_node(fat32_writer_t *w, fnode_t *parent, const char *comp, int len, int is_dir){
    (void)w;
    fnode_t *n=calloc(1,sizeof *n);
    n->is_dir=is_dir; n->parent=parent;
    if (len>255) len=255;
    memcpy(n->longname,comp,len); n->longlen=len;
    if (clean_83(comp,len,n->name83,&n->lcase)!=0){ n->needs_lfn=1; gen_alias(parent,comp,len,n->name83); }
    return n;
}
static void node_add(fnode_t *d, fnode_t *c){
    if (d->nchild==d->cap){ d->cap=d->cap?d->cap*2:8; d->child=realloc(d->child,d->cap*sizeof(*d->child)); }
    d->child[d->nchild++]=c;
}

static fnode_t *navigate(fat32_writer_t *w, const char *path, const char **last, int *last_len){
    const char *p=path; fnode_t *cur=w->root;
    while (*p){
        while (*p=='/'||*p=='\\') p++;
        if (!*p) break;
        const char *comp=p; int clen=0; while (*p&&*p!='/'&&*p!='\\'){ p++; clen++; }
        const char *peek=p; while (*peek=='/'||*peek=='\\') peek++;
        if (!*peek){ *last=comp; *last_len=clen; return cur; }
        fnode_t *nx=find_child(cur,comp,clen);
        if (!nx){ nx=make_node(w,cur,comp,clen,1); node_add(cur,nx); }
        if (!nx->is_dir) return NULL;
        cur=nx;
    }
    return NULL;
}

fat32_writer_t *fat32_open(blockio_t *io, uint64_t part_lba, uint64_t part_sectors, const char *label){
    fat32_writer_t *w=calloc(1,sizeof *w);
    w->io=io; w->part_off=part_lba*512; w->part_sectors=part_sectors;
    w->sectsz=512; w->spc=4; w->num_fats=2; w->reserved=32;
    w->cluster_size=w->sectsz*w->spc;
    w->serial=0x46415433u ^ (uint32_t)(part_sectors*2654435761u);
    uint64_t data_sectors=part_sectors - w->reserved;
    uint32_t clusters=(uint32_t)(data_sectors/w->spc);
    for (int it=0; it<8; it++){
        uint32_t fat_sectors=(uint32_t)(((uint64_t)(clusters+2)*4 + w->sectsz-1)/w->sectsz);
        w->fat_size=fat_sectors;
        uint64_t avail=part_sectors - w->reserved - (uint64_t)w->num_fats*fat_sectors;
        clusters=(uint32_t)(avail/w->spc);
    }
    w->total_clusters=clusters;
    if (clusters<65525){ free(w); return NULL; }
    w->fat_cap=clusters+2;
    w->fat=calloc(w->fat_cap,sizeof(uint32_t));
    w->fat[0]=0x0FFFFFF8; w->fat[1]=0x0FFFFFFF;
    w->next_free=2;
    w->root=calloc(1,sizeof(fnode_t)); w->root->is_dir=1;
    w->root->first_cluster=alloc_cluster(w);
    memset(w->label,' ',11); w->label[11]=0;
    if (label) for (int i=0;i<11&&label[i];i++){ char c=label[i]; w->label[i]=(c>='a'&&c<='z')?c-32:c; }
    return w;
}

int fat32_mkdir(fat32_writer_t *w, const char *path){
    const char *last; int llen; fnode_t *parent=navigate(w,path,&last,&llen);
    if (!parent) return -1;
    if (find_child(parent,last,llen)) return 0;
    node_add(parent, make_node(w,parent,last,llen,1));
    return 0;
}

int fat32_addfile(fat32_writer_t *w, const char *path, const void *data, uint64_t size){
    const char *last; int llen; fnode_t *parent=navigate(w,path,&last,&llen);
    if (!parent) return -1;
    fnode_t *f=make_node(w,parent,last,llen,0);
    f->size=(uint32_t)size;
    if (size){
        uint32_t nclu=(uint32_t)((size+w->cluster_size-1)/w->cluster_size), first=0, prev=0;
        for (uint32_t i=0;i<nclu;i++){ uint32_t c=alloc_cluster(w); if(!c)return -1; if(!first)first=c; else w->fat[prev]=c; prev=c; }
        f->first_cluster=first;
        uint64_t off=cluster_byte(w,first);
        blockio_write(w->io,off,data,size);
        uint64_t alloc=(uint64_t)nclu*w->cluster_size;
        if (alloc>size){ uint8_t z[512]={0}; uint64_t pad=alloc-size,base=off+size; while(pad){uint64_t k=pad<512?pad:512; blockio_write(w->io,base,z,k); base+=k; pad-=k;} }
    }
    node_add(parent,f);
    return 0;
}

static int lfn_count(const fnode_t *n){ return n->needs_lfn ? (n->longlen+12)/13 : 0; }

static int assign_dir_clusters(fat32_writer_t *w, fnode_t *d){
    uint32_t ndot = (d==w->root)?1:2;       /* root: volume label; else "."/".." */
    uint32_t ents = ndot;
    for (int i=0;i<d->nchild;i++) ents += 1 + lfn_count(d->child[i]);
    uint32_t bytes=ents*32;
    uint32_t nclu=(bytes+w->cluster_size-1)/w->cluster_size; if(!nclu)nclu=1;
    d->nclusters=nclu;
    if (d==w->root){
        uint32_t prev=d->first_cluster;
        for (uint32_t i=1;i<nclu;i++){ uint32_t c=alloc_cluster(w); if(!c)return -1; w->fat[prev]=c; prev=c; }
    } else {
        uint32_t first=0, prev=0;
        for (uint32_t i=0;i<nclu;i++){ uint32_t c=alloc_cluster(w); if(!c)return -1; if(!first)first=c; else w->fat[prev]=c; prev=c; }
        d->first_cluster=first;
    }
    for (int i=0;i<d->nchild;i++) if (d->child[i]->is_dir) if (assign_dir_clusters(w,d->child[i])) return -1;
    return 0;
}

static uint8_t lfn_checksum(const char name83[11]){
    uint8_t s=0; for (int i=0;i<11;i++) s=(uint8_t)(((s>>1)|(s<<7)) + (uint8_t)name83[i]); return s;
}
static void put_dirent(uint8_t *e, const char name83[11], uint8_t lcase, uint8_t attr, uint32_t clu, uint32_t size){
    memset(e,0,32); memcpy(e,name83,11);
    e[0x0B]=attr; e[0x0C]=lcase;
    w16(e+0x14,(uint16_t)(clu>>16)); w16(e+0x1A,(uint16_t)(clu&0xffff)); w32(e+0x1C,size);
}
static int emit_lfn(uint8_t *buf, const fnode_t *n){
    static const int pos[13]={1,3,5,7,9, 14,16,18,20,22,24, 28,30};
    int cnt=lfn_count(n); uint8_t sum=lfn_checksum(n->name83); int p=0;
    for (int e=cnt; e>=1; e--){
        uint8_t *d=buf+p; memset(d,0,32);
        d[0]=(uint8_t)(e | (e==cnt?0x40:0)); d[0x0B]=0x0F; d[0x0D]=sum;
        int base=(e-1)*13;
        for (int i=0;i<13;i++){ int ci=base+i; uint16_t ch;
            if (ci<n->longlen) ch=(uint8_t)n->longname[ci];
            else if (ci==n->longlen) ch=0x0000; else ch=0xFFFF;
            d[pos[i]]=ch&0xff; d[pos[i]+1]=ch>>8; }
        p+=32;
    }
    return p;
}

static void write_dir(fat32_writer_t *w, fnode_t *d){
    uint32_t cap=d->nclusters*w->cluster_size;
    uint8_t *buf=calloc(1,cap); uint32_t p=0;
    if (d==w->root){ put_dirent(buf+p,w->label,0,0x08,0,0); p+=32; }
    else {
        char dot[11]; memset(dot,' ',11); dot[0]='.'; put_dirent(buf+p,dot,0,0x10,d->first_cluster,0); p+=32;
        char dd[11]; memset(dd,' ',11); dd[0]='.'; dd[1]='.';
        uint32_t pc=(d->parent==w->root)?0:d->parent->first_cluster;
        put_dirent(buf+p,dd,0,0x10,pc,0); p+=32;
    }
    for (int i=0;i<d->nchild;i++){
        fnode_t *c=d->child[i];
        if (c->needs_lfn) p+=emit_lfn(buf+p,c);
        put_dirent(buf+p,c->name83,c->lcase,c->is_dir?0x10:0x20,c->first_cluster,c->is_dir?0:c->size); p+=32;
    }
    blockio_write(w->io, cluster_byte(w,d->first_cluster), buf, cap);
    free(buf);
    for (int i=0;i<d->nchild;i++) if (d->child[i]->is_dir) write_dir(w,d->child[i]);
}

int fat32_close(fat32_writer_t *w){
    if (assign_dir_clusters(w,w->root)) return -1;
    write_dir(w,w->root);

    uint32_t fat_bytes=w->fat_size*w->sectsz;
    uint8_t *fatbuf=calloc(1,fat_bytes);
    for (uint32_t i=0;i<w->next_free && i<w->fat_cap;i++) w32(fatbuf+i*4,w->fat[i]);
    for (uint32_t f=0; f<w->num_fats; f++)
        blockio_write(w->io, w->part_off+(uint64_t)(w->reserved+f*w->fat_size)*w->sectsz, fatbuf, fat_bytes);
    free(fatbuf);

    uint8_t bs[512]; memset(bs,0,512);
    bs[0]=0xEB; bs[1]=0x58; bs[2]=0x90; memcpy(bs+3,"MSDOS5.0",8);
    w16(bs+0x0B,w->sectsz); bs[0x0D]=(uint8_t)w->spc; w16(bs+0x0E,(uint16_t)w->reserved);
    bs[0x10]=(uint8_t)w->num_fats; bs[0x15]=0xF8; w16(bs+0x18,0x3F); w16(bs+0x1A,0xFF);
    w32(bs+0x1C,(uint32_t)(w->part_off/w->sectsz)); w32(bs+0x20,(uint32_t)w->part_sectors);
    w32(bs+0x24,w->fat_size); w32(bs+0x2C,2); w16(bs+0x30,1); w16(bs+0x32,6);
    bs[0x40]=0x80; bs[0x42]=0x29; w32(bs+0x43,w->serial);
    memcpy(bs+0x47,w->label,11); memcpy(bs+0x52,"FAT32   ",8); w16(bs+0x1FE,0xAA55);
    blockio_write(w->io,w->part_off,bs,512);
    blockio_write(w->io,w->part_off+6*512,bs,512);

    uint8_t fsi[512]; memset(fsi,0,512);
    w32(fsi+0x000,0x41615252); w32(fsi+0x1E4,0x61417272);
    w32(fsi+0x1E8,w->total_clusters-(w->next_free-2)); w32(fsi+0x1EC,w->next_free);
    w16(fsi+0x1FE,0xAA55);
    blockio_write(w->io,w->part_off+512,fsi,512);
    blockio_write(w->io,w->part_off+7*512,fsi,512);

    blockio_flush(w->io);
    return 0;
}
