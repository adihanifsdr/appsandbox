/*
 * AppSandboxSHM_producer.c — guest user-mode producer for the ivshmem shared-memory test.
 *
 * Opens our ivshmem driver, IOCTL-maps BAR2 into this process, and writes the SAME per-page
 * protocol that tools/shm-test/shm_host.c reads (magic + page header + payload + page0 control),
 * looping memcpy + frame_seq so the macOS host (mmap of the ivshmem backing file) can validate
 * integrity, measure host-read bandwidth, observe frame rate, and ping-pong round-trip latency.
 *
 * Build (user-mode, ARM64): vcvarsall arm64 && cl /O2 AppSandboxSHM_producer.c /link setupapi.lib
 * Run: AppSandboxSHM_producer.exe [seconds]
 */
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "AppSandboxSHM.h"
#pragma comment(lib, "setupapi.lib")

#define PAGE      4096
#define HDR       64
#define PAYLOAD   (PAGE - HDR)
#define MAGIC     0x53484D4652473031ULL   /* must match shm_host.c */
#define O_MAGIC 0
#define O_PIDX 8
#define O_PCNT 12
#define O_EPOCH 16
#define O_FSEQ 24
#define O_CRC 32
#define O_HOSTREQ 40
#define O_GUESTACK 48
#define O_GUESTHB 56

static uint32_t fnv32(const uint8_t *d, size_t n){
    uint32_t h = 2166136261u; for (size_t i=0;i<n;i++){ h ^= d[i]; h *= 16777619u; } return h;
}

static BYTE *map_bar(uint64_t *outSize){
    HDEVINFO di = SetupDiGetClassDevs(&GUID_DEVINTERFACE_APPSANDBOX_SHM, NULL, NULL,
                                      DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    SP_DEVICE_INTERFACE_DATA ifd; SP_DEVICE_INTERFACE_DETAIL_DATA *det; DWORD need=0;
    HANDLE h; APPSANDBOX_SHM_MAP m; DWORD br=0;
    if (di == INVALID_HANDLE_VALUE){ printf("SetupDiGetClassDevs err=%lu\n", GetLastError()); return NULL; }
    ifd.cbSize = sizeof(ifd);
    if (!SetupDiEnumDeviceInterfaces(di, NULL, &GUID_DEVINTERFACE_APPSANDBOX_SHM, 0, &ifd)){
        printf("EnumDeviceInterfaces err=%lu (driver bound? interface enabled?)\n", GetLastError()); return NULL; }
    SetupDiGetDeviceInterfaceDetail(di, &ifd, NULL, 0, &need, NULL);
    det = (SP_DEVICE_INTERFACE_DETAIL_DATA *)malloc(need);
    det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
    if (!SetupDiGetDeviceInterfaceDetail(di, &ifd, det, need, NULL, NULL)){
        printf("GetDeviceInterfaceDetail err=%lu\n", GetLastError()); return NULL; }
    h = CreateFile(det->DevicePath, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE,
                   NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE){ printf("CreateFile(%ws) err=%lu\n", det->DevicePath, GetLastError()); return NULL; }
    ZeroMemory(&m, sizeof(m));
    if (!DeviceIoControl(h, IOCTL_APPSANDBOX_SHM_MAP, NULL, 0, &m, sizeof(m), &br, NULL)){
        printf("IOCTL_APPSANDBOX_SHM_MAP err=%lu\n", GetLastError()); return NULL; }
    printf("MAPPED size=%llu userVa=0x%llx\n", (unsigned long long)m.size, (unsigned long long)m.userVa);
    *outSize = m.size;
    /* leak h on purpose: the mapping stays valid until the process exits (then the driver unmaps). */
    return (BYTE *)(uintptr_t)m.userVa;
}

int main(int argc, char **argv){
    int seconds = (argc > 1) ? atoi(argv[1]) : 60;
    uint64_t size = 0;
    BYTE *b = map_bar(&size);
    int pages, p, j;
    BYTE *src;
    LARGE_INTEGER qpf, qpc, start, lastRep;
    uint64_t epoch, fseq=0, lastReq=0, acks=0, lastFrames=0;
    if (!b) return 1;

    pages = 2025;                                  /* ~one 1080p frame of payload */
    if ((uint64_t)pages * PAGE > size) pages = (int)(size / PAGE);

    src = (BYTE *)malloc((size_t)pages * PAYLOAD);
    for (p=0;p<pages;p++){ BYTE *sp = src + (size_t)p*PAYLOAD; for (j=0;j<PAYLOAD;j++) sp[j] = (BYTE)((p*131 + j*7) & 0xFF); }

    QueryPerformanceFrequency(&qpf); QueryPerformanceCounter(&qpc); epoch = qpc.QuadPart;
    for (p=0;p<pages;p++){ BYTE *pg = b + (size_t)p*PAGE;
        *(uint64_t*)(pg+O_MAGIC)=MAGIC; *(uint32_t*)(pg+O_PIDX)=p; *(uint32_t*)(pg+O_PCNT)=pages;
        *(uint64_t*)(pg+O_EPOCH)=epoch; *(uint64_t*)(pg+O_FSEQ)=0;
        memcpy(pg+HDR, src+(size_t)p*PAYLOAD, PAYLOAD); *(uint32_t*)(pg+O_CRC)=fnv32(pg+HDR, PAYLOAD); }
    *(uint64_t*)(b+O_HOSTREQ)=0; *(uint64_t*)(b+O_GUESTACK)=0; *(uint64_t*)(b+O_GUESTHB)=0;
    MemoryBarrier();
    printf("READY epoch=%llu pages=%d\n", (unsigned long long)epoch, pages); fflush(stdout);

    QueryPerformanceCounter(&start); lastRep = start;
    for (;;){
        QueryPerformanceCounter(&qpc);
        if (qpc.QuadPart - start.QuadPart >= (LONGLONG)seconds * qpf.QuadPart) break;
        fseq++;
        for (p=0;p<pages;p++){ BYTE *pg = b + (size_t)p*PAGE;
            memcpy(pg+HDR, src+(size_t)p*PAYLOAD, PAYLOAD);
            *(uint64_t*)(pg+O_FSEQ)=fseq;
            if ((p & 15)==0){ MemoryBarrier();
                uint64_t req=*(volatile uint64_t*)(b+O_HOSTREQ);
                if (req!=lastReq){ *(volatile uint64_t*)(b+O_GUESTACK)=req; MemoryBarrier(); lastReq=req; acks++; } } }
        *(uint64_t*)(b+O_GUESTHB)=fseq;
        MemoryBarrier();
        { uint64_t req2=*(volatile uint64_t*)(b+O_HOSTREQ);
          if (req2!=lastReq){ *(volatile uint64_t*)(b+O_GUESTACK)=req2; MemoryBarrier(); lastReq=req2; acks++; } }
        if (qpc.QuadPart - lastRep.QuadPart >= qpf.QuadPart){
            double dt = (double)(qpc.QuadPart - lastRep.QuadPart)/qpf.QuadPart;
            uint64_t df = fseq - lastFrames;
            double mb = (double)df * pages * PAYLOAD / dt / (1024*1024);
            printf("t=%llds frame=%llu fps=%lld writeMBps=%lld acks=%llu\n",
                   (long long)((qpc.QuadPart-start.QuadPart)/qpf.QuadPart), (unsigned long long)fseq,
                   (long long)(df/dt), (long long)mb, (unsigned long long)acks);
            fflush(stdout); lastRep = qpc; lastFrames = fseq; }
    }
    printf("DONE frames=%llu acks=%llu\n", (unsigned long long)fseq, (unsigned long long)acks);
    return 0;
}
