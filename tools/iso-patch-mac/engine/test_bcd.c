/* test_bcd.c -- build a BCD from BCD-Template with recognizable GUIDs. */
#include "bcd_patch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc,char**argv){
    const char *tin = argc>1?argv[1]:"/tmp/BCD-Template";
    const char *tout= argc>2?argv[2]:"/tmp/BCD-out";
    FILE *f=fopen(tin,"rb"); if(!f){printf("open %s fail\n",tin);return 2;}
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *tpl=malloc(n); if(fread(tpl,1,n,f)!=(size_t)n){return 2;} fclose(f);

    bcd_guids_t g; memset(&g,0,sizeof g);
    for(int i=0;i<16;i++){ g.disk_guid[i]=0xD0+i; g.esp_part_guid[i]=0xE0+i; g.win_part_guid[i]=0x10+i; }

    uint8_t *ob; size_t ol;
    int rc=bcd_build(tpl,(size_t)n,&g,&ob,&ol);
    printf("bcd_build rc=%d  out_len=%zu  (template=%ld)\n",rc,ol,n);
    if(rc) return 1;
    FILE *o=fopen(tout,"wb"); fwrite(ob,1,ol,o); fclose(o);
    printf("wrote %s\n",tout);
    return 0;
}
