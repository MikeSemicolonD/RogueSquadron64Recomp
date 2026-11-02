// Factor 5's profiler HUD (F5 key) laid its bars out in 320 pixels of the current framebuffer: 62% of a mission's 512x448 picture, 50% of a cutscene's 640x340, taller there.
// emitProfilerBarSegment command for command, x scaled by width/512 and rows by 512/width (a cutscene pixel is as much taller on screen as narrower), so missions keep the game's size.
#include "rs64_api.h"

#define PROFILER_GATE (*(volatile unsigned char*)0x80038CE0)
#define PROFILER_TICKS_PER_SEC (*(volatile unsigned long long*)0x80039100)
#define DL_CHUNK_END (*(unsigned int**)0x801163D4)
#define FB_MAX_X (*(volatile unsigned short*)0x8011A90E)
#define FB_MAX_Y (*(volatile unsigned short*)0x8011A910)

unsigned long long u64DivU64(unsigned long long num, unsigned long long den);
unsigned int* allocateDisplayListBuffer(unsigned int* dl);

static unsigned int* emit(unsigned int* dl, unsigned int w0, unsigned int w1) {
    if (!(dl < DL_CHUNK_END)) {
        dl[0] = 0xB5000000u;
        dl[1] = 0;
        dl = allocateDisplayListBuffer(dl);
    }
    dl[0] = w0;
    dl[1] = w1;
    return dl + 2;
}

RECOMP_PATCH unsigned int* emitProfilerBarSegment(unsigned int* dl, unsigned int ticks, short y, int r, int g, int b) {
    if (PROFILER_GATE == 0) {
        return dl;
    }

    unsigned long long q = u64DivU64((unsigned long long)ticks * 1000000000ull, PROFILER_TICKS_PER_SEC);
    unsigned int len = (unsigned int)u64DivU64(q << 5, 3333333ull);
    if (len > 320) {
        len = 320;
    }

    const unsigned int width = (unsigned int)FB_MAX_X + 1, maxY = FB_MAX_Y;
    const int off = (int)maxY - y;
    // 2 pixels in from the left: a fill touching the scissor's left edge is stretched out to the widened screen edge in widescreen.
    const unsigned int x0 = 2, x160 = x0 + 160 * width / 512, x161 = x0 + 161 * width / 512, x320 = x0 + 320 * width / 512, xlen = x0 + len * width / 512;
    // Float: an integer divide by a variable emits a teq trap that N64Recomp cannot translate.
    const float rowScale = 512.0f / (float)width;
    const int rowOff = (int)((float)off * rowScale), rowHeight = (int)(3.0f * rowScale + 0.5f);
    const unsigned int y0 = (unsigned int)((int)maxY - rowOff) & 0x3FF;
    const unsigned int y1 = (unsigned int)((int)maxY - rowOff + (rowHeight > 0 ? rowHeight : 1)) & 0x3FF;
    const unsigned int c = ((r << 8) & 0xF800) | ((g << 3) & 0x7C0) | ((b >> 2) & 0x3E) | 1;

    dl = emit(dl, 0xE7000000u, 0);
    dl = emit(dl, 0xB900031Du, 0x0F0A4000u);
    dl = emit(dl, 0xBA001402u, 0x00300000u);
    dl = emit(dl, 0xF7000000u, 0x42114211u);
    dl = emit(dl, 0xF6000000u | ((x160 & 0x3FF) << 14) | (y1 << 2), (x0 << 14) | (y0 << 2));
    dl = emit(dl, 0xE7000000u, 0);
    dl = emit(dl, 0xF7000000u, 0x21092109u);
    dl = emit(dl, 0xF6000000u | ((x320 & 0x3FF) << 14) | (y1 << 2), ((x161 & 0x3FF) << 14) | (y0 << 2));
    dl = emit(dl, 0xE7000000u, 0);
    dl = emit(dl, 0xF7000000u, (c << 16) | c);
    return emit(dl, 0xF6000000u | ((xlen & 0x3FF) << 14) | (y1 << 2), (x0 << 14) | (y0 << 2));
}
