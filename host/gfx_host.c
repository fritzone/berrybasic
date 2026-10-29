// Host (Linux) backend for the seed graphics primitives (gfx.h). The host has no
// framebuffer, so every drawing call is a no-op and sgfx_avail() reports 0; a
// portable seed checks that first. Text metrics still work through ttf_host.c
// (reached separately by basic.c), so a seed can lay out text on the host too.
#include <stdint.h>
#include "gfx.h"

int      sgfx_avail(void)  { return 0; }
int      sgfx_width(void)  { return 0; }
int      sgfx_height(void) { return 0; }
void     sgfx_clear(uint32_t rgb) { (void)rgb; }
void     sgfx_putpixel(int x, int y, uint32_t rgb) { (void)x; (void)y; (void)rgb; }
void     sgfx_blit8(const unsigned char *idx, const unsigned int *pal, int w, int h, int dx, int dy, int scale) { (void)idx; (void)pal; (void)w; (void)h; (void)dx; (void)dy; (void)scale; }
uint32_t sgfx_getpixel(int x, int y) { (void)x; (void)y; return 0; }
void     sgfx_line(int x1, int y1, int x2, int y2, uint32_t rgb) { (void)x1; (void)y1; (void)x2; (void)y2; (void)rgb; }
void     sgfx_fillrect(int x1, int y1, int x2, int y2, uint32_t rgb) { (void)x1; (void)y1; (void)x2; (void)y2; (void)rgb; }
void     sgfx_circle(int cx, int cy, int r, uint32_t rgb) { (void)cx; (void)cy; (void)r; (void)rgb; }
void     sgfx_fillcircle(int cx, int cy, int r, uint32_t rgb) { (void)cx; (void)cy; (void)r; (void)rgb; }
void     sgfx_ellipse(int cx, int cy, int rx, int ry, uint32_t rgb) { (void)cx; (void)cy; (void)rx; (void)ry; (void)rgb; }
void     sgfx_fillellipse(int cx, int cy, int rx, int ry, uint32_t rgb) { (void)cx; (void)cy; (void)rx; (void)ry; (void)rgb; }
void     sgfx_fillpoly(const int *xy, int npts, uint32_t rgb) { (void)xy; (void)npts; (void)rgb; }
void     sgfx_line_style(int w, int j, int c) { (void)w; (void)j; (void)c; }
void     sgfx_polyline(const int *pts, int n, int cl, uint32_t rgb) { (void)pts; (void)n; (void)cl; (void)rgb; }
void     sgfx_flood(int x, int y, uint32_t rgb) { (void)x; (void)y; (void)rgb; }
void     sgfx_clip(int x1, int y1, int x2, int y2) { (void)x1; (void)y1; (void)x2; (void)y2; }
void     sgfx_noclip(void) { }
void     sgfx_text(int x, int y, const char *s, int len, uint32_t rgb) { (void)x; (void)y; (void)s; (void)len; (void)rgb; }
void     sgfx_font(int *w, int *h) { if (w) *w = 0; if (h) *h = 0; }
void     sgfx_glyph(int px, int py, int ch, uint32_t fg, uint32_t bg) { (void)px; (void)py; (void)ch; (void)fg; (void)bg; }
