#include "graphics.h"

static uint32_t *fb_buf;
static uint32_t  fb_width;
static uint32_t  fb_height;
static uint32_t  fb_pitch_words;

void init_graphics(uint32_t *fb, uint32_t w, uint32_t h, uint32_t pitch) {
    fb_buf         = fb;
    fb_width       = w;
    fb_height      = h;
    fb_pitch_words = pitch >> 2;
}

void putpixel(int x, int y, uint32_t color) {
    if ((unsigned)x >= fb_width || (unsigned)y >= fb_height) return;
    fb_buf[y * fb_pitch_words + x] = color;
}

uint32_t getpixel(int x, int y) {
    if ((unsigned)x >= fb_width || (unsigned)y >= fb_height) return 0;
    return fb_buf[y * fb_pitch_words + x];
}

// --- Graphics plot operation -----------------------------------------------
// The BBC GCOL mode selects how a plotted colour combines with the screen.
static int g_op = 0;   // 0=store 1=OR 2=AND 3=EOR 4=invert

void gfx_set_op(int op) { g_op = op; }

// Pixel write that applies the current plot op (used by the graphics primitives;
// the text console keeps using the plain putpixel above).
static void gpix(int x, int y, uint32_t c) {
    if ((unsigned)x >= fb_width || (unsigned)y >= fb_height) return;
    uint32_t *d = &fb_buf[y * fb_pitch_words + x];
    switch (g_op) {
        case 1: *d |= c;                       break;   // OR
        case 2: *d &= c;                       break;   // AND
        case 3: *d ^= (c & 0x00FFFFFFu);       break;   // EOR (keep alpha)
        case 4: *d = (~*d) | 0xFF000000u;      break;   // invert (colour ignored)
        default: *d = c;                       break;   // store
    }
}

void draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;
    for (;;) {
        gpix(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err << 1;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void fill_rect(int x0, int y0, int x1, int y1, uint32_t color) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            gpix(x, y, color);
}

// Horizontal span helper for the triangle scan-fill.
static void hspan(int xa, int xb, int y, uint32_t color) {
    if (xa > xb) { int t = xa; xa = xb; xb = t; }
    for (int x = xa; x <= xb; x++) gpix(x, y, color);
}

void fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color) {
    // Sort vertices by ascending y (y0<=y1<=y2).
    if (y0 > y1) { int t; t=y0;y0=y1;y1=t; t=x0;x0=x1;x1=t; }
    if (y0 > y2) { int t; t=y0;y0=y2;y2=t; t=x0;x0=x2;x2=t; }
    if (y1 > y2) { int t; t=y1;y1=y2;y2=t; t=x1;x1=x2;x2=t; }
    if (y2 == y0) {            // degenerate (all on one scanline)
        int lo = x0, hi = x0;
        if (x1 < lo) lo = x1;
        if (x2 < lo) lo = x2;
        if (x1 > hi) hi = x1;
        if (x2 > hi) hi = x2;
        hspan(lo, hi, y0, color);
        return;
    }
    for (int y = y0; y <= y2; y++) {
        int xl = x0 + (x2 - x0) * (y - y0) / (y2 - y0);   // long edge v0->v2
        int xs;                                           // short edge
        if (y < y1) xs = (y1 == y0) ? x1 : x0 + (x1 - x0) * (y - y0) / (y1 - y0);
        else        xs = (y2 == y1) ? x1 : x1 + (x2 - x1) * (y - y1) / (y2 - y1);
        hspan(xl, xs, y, color);
    }
}

void draw_circle(int cx, int cy, int r, uint32_t color) {
    if (r < 0) r = -r;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gpix(cx + x, cy + y, color); gpix(cx - x, cy + y, color);
        gpix(cx + x, cy - y, color); gpix(cx - x, cy - y, color);
        gpix(cx + y, cy + x, color); gpix(cx - y, cy + x, color);
        gpix(cx + y, cy - x, color); gpix(cx - y, cy - x, color);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void fill_circle(int cx, int cy, int r, uint32_t color) {
    if (r < 0) r = -r;
    for (int y = -r; y <= r; y++) {
        int dx = 0;
        while (dx * dx + y * y <= r * r) dx++;   // disc half-width at this row
        dx--;
        hspan(cx - dx, cx + dx, cy + y, color);
    }
}

void bar(int x1, int y1, int x2, int y2, uint32_t color) {
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    for (int y = y1; y <= y2; y++)
        for (int x = x1; x <= x2; x++)
            putpixel(x, y, color);
}

void cleardevice(void) {
    uint32_t words = fb_pitch_words * fb_height;
    for (uint32_t i = 0; i < words; i++)
        fb_buf[i] = COLOR_BLACK;
}

void scroll_up(int pixels, uint32_t fill) {
    if (pixels <= 0) return;
    if ((uint32_t)pixels >= fb_height) { cleardevice(); return; }

    uint32_t keep = fb_height - (uint32_t)pixels;

    // Move the rows that survive up by `pixels` scanlines.
    for (uint32_t y = 0; y < keep; y++) {
        uint32_t *dst = fb_buf + (uint64_t)y * fb_pitch_words;
        uint32_t *src = fb_buf + (uint64_t)(y + pixels) * fb_pitch_words;
        for (uint32_t x = 0; x < fb_width; x++) dst[x] = src[x];
    }
    // Clear the freshly exposed rows at the bottom.
    for (uint32_t y = keep; y < fb_height; y++) {
        uint32_t *dst = fb_buf + (uint64_t)y * fb_pitch_words;
        for (uint32_t x = 0; x < fb_width; x++) dst[x] = fill;
    }
}
