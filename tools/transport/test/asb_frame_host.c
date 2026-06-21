/* asb_frame_host.c — macOS host: read the ivshmem ch2 frame region (BGRA) and save frames as BMP.
 * Lays out the directory with a frame region, waits for the guest to publish frames, dumps N of them.
 * Build: cc -O2 -o /tmp/asb_frame_host asb_frame_host.c   Run: asb_frame_host <ivshmem.bin> [out_prefix] [N]
 * Proves the shared-memory frame channel carries real guest pixels (the VDD will write the same region).*/
#include "../asb_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define FRAME_REGION_OFF  0x10000ull          /* 64 KiB (past the directory page) */
#define FRAME_REGION_SIZE (48ull * 1024 * 1024) /* 48 MiB: 1080p double/triple-buffer + headroom */

static void write_bmp(const char *path, const uint8_t *bgra, int w, int h, int stride) {
    FILE *f = fopen(path, "wb"); if (!f) { perror("fopen"); return; }
    uint32_t img = (uint32_t)stride * h, filesz = 54 + img; int y;
    uint8_t fh[14] = {'B','M'}; uint8_t ih[40] = {0};
    memcpy(fh+2,&filesz,4); uint32_t off=54; memcpy(fh+10,&off,4);
    uint32_t v=40; memcpy(ih,&v,4); memcpy(ih+4,&w,4); int nh=-h; memcpy(ih+8,&nh,4); /* top-down */
    uint16_t planes=1,bpp=32; memcpy(ih+12,&planes,2); memcpy(ih+14,&bpp,2);
    memcpy(ih+20,&img,4);
    fwrite(fh,1,14,f); fwrite(ih,1,40,f);
    for (y=0;y<h;y++) fwrite(bgra + (size_t)y*stride, 1, stride, f);  /* BGRA rows = BMP 32bpp */
    fclose(f);
}

int main(int argc, char **argv) {
    const char *path = argc>1?argv[1]:"/tmp/ivshmem.bin";
    const char *pre  = argc>2?argv[2]:"/tmp/frame";
    int N = argc>3?atoi(argv[3]):3;
    int fd = open(path, O_RDWR); struct stat st; uint8_t *bar; AsbShmDirectory *dir;
    AsbFrameRegion *fr; uint8_t *region; int got=0; uint32_t last_seq=0; int waited=0;
    if (fd<0){ perror("open"); return 1; }
    fstat(fd,&st); bar = mmap(NULL, st.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (bar==MAP_FAILED){ perror("mmap"); return 1; }
    dir = (AsbShmDirectory*)bar;
    /* publish a directory with a single ch2 FRAME region */
    memset(bar, 0, 4096);
    dir->version = ASB_SHM_VERSION; dir->bar_size = (uint32_t)st.st_size; dir->n_regions = 1;
    dir->regions[0].channel_id = ASB_CH_DISPLAY; dir->regions[0].flags = ASB_REGION_FRAME;
    dir->regions[0].offset = FRAME_REGION_OFF; dir->regions[0].size = FRAME_REGION_SIZE;
    region = bar + FRAME_REGION_OFF;
    fr = (AsbFrameRegion*)region;
    fr->magic = 0;                        /* clear; guest's asb_frame_open will set it */
    __sync_synchronize();
    dir->magic = ASB_SHM_DIR_MAGIC; __sync_synchronize();
    printf("[host] directory published: ch2 frame region @0x%llx (%llu MiB). Waiting for guest frames...\n",
           FRAME_REGION_OFF, FRAME_REGION_SIZE/(1024*1024));

    while (got < N && waited < 240000) {   /* 240s: leave time to install + start the VDD */
        __sync_synchronize();
        if (fr->magic == 0x52465341u && fr->produced_seq != last_seq && fr->width && fr->height) {
            uint32_t seq = fr->produced_seq, ab = fr->active_buffer;
            int w = (int)fr->width, h = (int)fr->height, stride = (int)fr->stride;
            const uint8_t *buf = region + fr->buffers_offset + (uint64_t)ab*fr->buffer_stride;
            char out[512]; snprintf(out, sizeof(out), "%s-%u.bmp", pre, seq);
            __sync_synchronize();
            write_bmp(out, buf, w, h, stride);
            /* simple "is it real?" signal: average luma of a center sample */
            unsigned long long s=0; int n=0; int yy,xx;
            for (yy=h/2-2; yy<h/2+2; yy++) for (xx=0; xx<w; xx+=37) {
                const uint8_t*p=buf+(size_t)yy*stride+(size_t)xx*4; s += p[0]+p[1]+p[2]; n++; }
            printf("[host] frame seq=%u %dx%d stride=%d buf=%u -> %s  (center avg=%llu, %s)\n",
                   seq, w, h, stride, ab, out, n?s/(3ull*n):0,
                   (n && s/(3ull*n) > 2) ? "non-black ✓" : "black/empty");
            last_seq = seq; got++;
        }
        usleep(5000); waited += 5;
    }
    printf("[host] done (%d frames dumped).\n", got);
    return got ? 0 : 2;
}
