#include "graphics.h"
#include "font.h"

static uint32_t *fb_buf;
static uint32_t  fb_width;
static uint32_t  fb_height;
static uint32_t  fb_pitch_words;

// Optional clip rectangle (in physical pixels) for the graphics viewport (VDU
// 24). When clip_on is 0 the whole framebuffer is writable.
static int clip_on = 0;
static int clip_x0, clip_y0, clip_x1, clip_y1;

void gfx_set_clip(int x0, int y0, int x1, int y1) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    clip_x0 = x0; clip_y0 = y0; clip_x1 = x1; clip_y1 = y1; clip_on = 1;
}
void gfx_clear_clip(void) { clip_on = 0; }
void gfx_clip_rect(int *x0, int *y0, int *x1, int *y1) {
    if (clip_on) { *x0 = clip_x0; *y0 = clip_y0; *x1 = clip_x1; *y1 = clip_y1; }
    else         { *x0 = 0; *y0 = 0; *x1 = (int)fb_width - 1; *y1 = (int)fb_height - 1; }
}

static int clipped(int x, int y) {
    if ((unsigned)x >= fb_width || (unsigned)y >= fb_height) return 1;
    if (clip_on && (x < clip_x0 || x > clip_x1 || y < clip_y0 || y > clip_y1)) return 1;
    return 0;
}

void init_graphics(uint32_t *fb, uint32_t w, uint32_t h, uint32_t pitch) {
    fb_buf         = fb;
    fb_width       = w;
    fb_height      = h;
    fb_pitch_words = pitch >> 2;
    clip_on        = 0;
}

void putpixel(int x, int y, uint32_t color) {
    if (clipped(x, y)) return;
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
    if (clipped(x, y)) return;
    uint32_t *d = &fb_buf[y * fb_pitch_words + x];
    switch (g_op) {
        case 1: *d |= c;                       break;   // OR
        case 2: *d &= c;                       break;   // AND
        case 3: *d ^= (c & 0x00FFFFFFu);       break;   // EOR (keep alpha)
        case 4: *d = (~*d) | 0xFF000000u;      break;   // invert (colour ignored)
        default: *d = c;                       break;   // store
    }
}

void putpixel_op(int x, int y, uint32_t color) { gpix(x, y, color); }

// Draw one glyph (GLYPH_BYTES row bytes, MSB = leftmost pixel) at (x,y), each
// source pixel scaled to `scale`x`scale`, using the current plot op (gfx_set_op)
// and the clip rectangle. Only set bits are drawn, so the background stays
// transparent. Used for VDU 5 (text at the graphics cursor).
void draw_glyph_op(const unsigned char *glyph, int x, int y, int scale, uint32_t color) {
    for (int row = 0; row < GLYPH_H; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < GLYPH_W; col++) {
            if (!(bits & (0x80 >> col))) continue;
            int px = x + col * scale, py = y + row * scale;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++) gpix(px + dx, py + dy, color);
        }
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

// Scroll a rectangular region [x0,y0]-[x1,y1] (inclusive, physical pixels) up by
// `pixels` rows, filling the exposed bottom rows with `fill`. Used to scroll a
// text viewport (VDU 28) that is smaller than the whole screen.
void scroll_rect(int x0, int y0, int x1, int y1, int pixels, uint32_t fill) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= (int)fb_width)  x1 = (int)fb_width - 1;
    if (y1 >= (int)fb_height) y1 = (int)fb_height - 1;
    if (pixels <= 0 || y0 > y1 || x0 > x1) return;
    for (int y = y0; y <= y1; y++) {
        if (y + pixels <= y1) {
            uint32_t *dst = fb_buf + (uint64_t)y * fb_pitch_words;
            uint32_t *src = fb_buf + (uint64_t)(y + pixels) * fb_pitch_words;
            for (int x = x0; x <= x1; x++) dst[x] = src[x];
        } else {
            uint32_t *dst = fb_buf + (uint64_t)y * fb_pitch_words;
            for (int x = x0; x <= x1; x++) dst[x] = fill;
        }
    }
}

// Shift the content of rectangle [x0,y0]-[x1,y1] (inclusive, physical pixels) by
// (dx,dy) pixels, filling exposed cells with `fill`. Copy order is chosen so the
// source isn't overwritten before it's read. Used by VDU 23,7 (scroll window).
void move_rect(int x0, int y0, int x1, int y1, int dx, int dy, uint32_t fill) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= (int)fb_width)  x1 = (int)fb_width - 1;
    if (y1 >= (int)fb_height) y1 = (int)fb_height - 1;
    if (x0 > x1 || y0 > y1) return;
    int ys = (dy > 0) ? y1 : y0, ye = (dy > 0) ? y0 - 1 : y1 + 1, ystep = (dy > 0) ? -1 : 1;
    int xs = (dx > 0) ? x1 : x0, xe = (dx > 0) ? x0 - 1 : x1 + 1, xstep = (dx > 0) ? -1 : 1;
    for (int y = ys; y != ye; y += ystep) {
        int sy = y - dy;
        for (int x = xs; x != xe; x += xstep) {
            int sx = x - dx;
            uint32_t v = (sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1)
                       ? fb_buf[(uint64_t)sy * fb_pitch_words + sx] : fill;
            fb_buf[(uint64_t)y * fb_pitch_words + x] = v;
        }
    }
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

// ---------------------------------------------------------------------------
// Extended primitives for the BASIC graphics library: rectangle outline,
// ellipse (outline + fill), and flood fill. All honour the clip rectangle; the
// outline/fill shapes go through gpix() so the current plot op (OR/AND/EOR/
// invert) applies, exactly like the built-in line/rectangle/circle.
// ---------------------------------------------------------------------------

static long gsqrt(long v) {                 // integer sqrt (Newton), v >= 0
    if (v <= 0) return 0;
    long x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return x;
}

void draw_rect(int x0, int y0, int x1, int y1, uint32_t color) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int x = x0; x <= x1; x++) { gpix(x, y0, color); gpix(x, y1, color); }
    for (int y = y0; y <= y1; y++) { gpix(x0, y, color); gpix(x1, y, color); }
}

void fill_ellipse(int cx, int cy, int rx, int ry, uint32_t color) {
    if (rx < 0) rx = -rx;
    if (ry < 0) ry = -ry;
    if (ry == 0) { hspan(cx - rx, cx + rx, cy, color); return; }
    long rx2 = (long)rx * rx, ry2 = (long)ry * ry;
    for (int dy = 0; dy <= ry; dy++) {
        int xr = (int)gsqrt(rx2 * (ry2 - (long)dy * dy) / ry2);
        hspan(cx - xr, cx + xr, cy - dy, color);
        if (dy != 0) hspan(cx - xr, cx + xr, cy + dy, color);
    }
}

void draw_ellipse(int cx, int cy, int rx, int ry, uint32_t color) {
    if (rx < 0) rx = -rx;
    if (ry < 0) ry = -ry;
    if (ry == 0) { hspan(cx - rx, cx + rx, cy, color); return; }
    if (rx == 0) { for (int y = cy - ry; y <= cy + ry; y++) gpix(cx, y, color); return; }
    long rx2 = (long)rx * rx, ry2 = (long)ry * ry;
    // Two scan passes (by dy and by dx) so the outline never has gaps on the
    // flat top/bottom or the steep sides.
    for (int dy = 0; dy <= ry; dy++) {
        int xr = (int)gsqrt(rx2 * (ry2 - (long)dy * dy) / ry2);
        gpix(cx - xr, cy - dy, color); gpix(cx + xr, cy - dy, color);
        gpix(cx - xr, cy + dy, color); gpix(cx + xr, cy + dy, color);
    }
    for (int dx = 0; dx <= rx; dx++) {
        int yr = (int)gsqrt(ry2 * (rx2 - (long)dx * dx) / rx2);
        gpix(cx - dx, cy - yr, color); gpix(cx + dx, cy - yr, color);
        gpix(cx - dx, cy + yr, color); gpix(cx + dx, cy + yr, color);
    }
}

// Scanline flood fill. A fixed span-seed stack bounds memory; seeding one entry
// per contiguous run keeps it small. Matches by RGB (alpha ignored). Best effort:
// if the stack fills on a huge region it simply stops early.
#define FLOOD_CAP 8192
void flood_fill(int sx, int sy, uint32_t newc) {
    int cx0, cy0, cx1, cy1;
    gfx_clip_rect(&cx0, &cy0, &cx1, &cy1);
    if (sx < cx0 || sx > cx1 || sy < cy0 || sy > cy1) return;
    uint32_t target = getpixel(sx, sy) & 0x00FFFFFFu;
    if (target == (newc & 0x00FFFFFFu)) return;         // nothing to do

    static int stk[FLOOD_CAP * 2];
    int sp = 0;
    stk[sp++] = sx; stk[sp++] = sy;
    while (sp) {
        int y = stk[--sp];
        int x = stk[--sp];
        if ((getpixel(x, y) & 0x00FFFFFFu) != target) continue;
        int xl = x, xr = x;
        while (xl > cx0 && (getpixel(xl - 1, y) & 0x00FFFFFFu) == target) xl--;
        while (xr < cx1 && (getpixel(xr + 1, y) & 0x00FFFFFFu) == target) xr++;
        for (int i = xl; i <= xr; i++) putpixel(i, y, newc);
        for (int ny = y - 1; ny <= y + 1; ny += 2) {
            if (ny < cy0 || ny > cy1) continue;
            int i = xl;
            while (i <= xr) {
                if ((getpixel(i, ny) & 0x00FFFFFFu) == target) {
                    if (sp < FLOOD_CAP * 2 - 2) { stk[sp++] = i; stk[sp++] = ny; }
                    while (i <= xr && (getpixel(i, ny) & 0x00FFFFFFu) == target) i++;
                } else i++;
            }
        }
    }
}
