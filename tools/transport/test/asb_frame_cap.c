/* asb_frame_cap.c — guest: capture the real desktop (GDI) into the ivshmem ch2 frame region.
 * Standalone validation harness — proves the shared-memory frame channel carries real pixels to the
 * host BEFORE wiring the same write into the VDD. Does NOT touch any existing code path.
 * Build: cl /O2 asb_frame_cap.c ..\asb_transport.c /link gdi32.lib user32.lib ws2_32.lib setupapi.lib advapi32.lib
 * Run:   asb_frame_cap.exe [seconds] */
#include "../asb_transport.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    int seconds = argc>1?atoi(argv[1]):8;
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    AsbFrame *f; HDC hScreen, hDC; HBITMAP hBmp; void *pBits = NULL; BITMAPINFO bi = {0};
    DWORD t0; int frames = 0;

    if (asb_transport_init() != 0) { printf("init failed\n"); return 1; }
    printf("backend=%s screen=%dx%d\n", asb_transport_is_ivshmem()?"ivshmem":"hyperv", w, h);
    if (!asb_transport_is_ivshmem()) { printf("not ivshmem (no directory/region) — host harness running?\n"); return 1; }

    f = asb_frame_open(w, h, 2);
    if (!f) { printf("asb_frame_open failed (ch2 frame region present?)\n"); return 1; }
    printf("frame channel open (%dx%d, double-buffered). Capturing desktop...\n", w, h);

    hScreen = GetDC(NULL);
    hDC = CreateCompatibleDC(hScreen);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;   /* top-down */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    hBmp = CreateDIBSection(hScreen, &bi, DIB_RGB_COLORS, &pBits, NULL, 0);
    SelectObject(hDC, hBmp);

    t0 = GetTickCount();
    while ((int)((GetTickCount()-t0)/1000) < seconds) {
        BitBlt(hDC, 0, 0, w, h, hScreen, 0, 0, SRCCOPY);    /* real desktop pixels → pBits (BGRA) */
        {
            void *back = asb_frame_back_buffer(f);
            if (back) { memcpy(back, pBits, (size_t)w*h*4); asb_frame_publish(f); frames++; }
        }
        Sleep(33);   /* ~30 fps */
    }
    printf("DONE published %d frames\n", frames);
    DeleteObject(hBmp); DeleteDC(hDC); ReleaseDC(NULL, hScreen);
    asb_frame_close(f);
    return 0;
}
