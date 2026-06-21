/* test_fat32.c -- author an ESP-like FAT32 volume for fsck_msdos / mount checks. */
#include "fat32.h"
#include "blockio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc,char**argv){
    const char*path = argc>1?argv[1]:"/tmp/esp.img";
    uint64_t bytes = 200ull<<20;          /* 200 MiB ESP */
    blockio_t*io=blockio_create(path,bytes);
    if(!io){printf("blockio_create fail\n");return 2;}
    fat32_writer_t*w=fat32_open(io,0,bytes/512,"ESP");
    if(!w){printf("fat32_open fail\n");return 2;}

    fat32_mkdir(w,"/EFI/Boot");
    fat32_mkdir(w,"/EFI/Microsoft/Boot/Fonts");

    static uint8_t a[3030944]; for(size_t i=0;i<sizeof a;i++) a[i]=(uint8_t)(i*131+7);
    fat32_addfile(w,"/EFI/Boot/bootaa64.efi",a,sizeof a);
    fat32_addfile(w,"/EFI/Microsoft/Boot/bootmgfw.efi",a,sizeof a);
    fat32_addfile(w,"/EFI/Microsoft/Boot/BCD","\x72\x65\x67\x66 fake-bcd",13);
    fat32_addfile(w,"/EFI/Microsoft/Boot/memtest.efi","hello",5);

    int rc=fat32_close(w);
    printf("fat32_close rc=%d\n",rc);
    blockio_close(io);
    return rc?1:0;
}
