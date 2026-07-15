#ifndef GFX_H
#define GFX_H

#include <stdint.h>

// Physical-pixel graphics primitives used to back the native seeds' BGI-style
// <graphics.h> (see seed/include/graphics.h and the ABI v7 additions in seed.h).
//
// Unlike BASIC's con_* graphics — which work in BBC logical coordinates (0..1279
// x 0..1023, origin bottom-left) — these address the framebuffer directly in
// device pixels: origin TOP-LEFT, x right, y down, matching how most modern graphics
// count. Colours are 24-bit 0xRRGGBB. Drawing lands
// on whatever surface is currently active (the visible screen or a BASIC
// BUFFER/SPRITETARGET), so a seed's output composes with BASIC's.
//
// Two backends implement this: the framebuffer target (kernel.c, forwarding to
// the graphics.c driver) and a host stub (host/gfx_host.c) where every call is a
// no-op and sgfx_avail() is 0.

int      sgfx_avail(void);                  // 1 if a framebuffer is present, else 0
int      sgfx_width(void);                  // screen width in pixels
int      sgfx_height(void);                 // screen height in pixels
void     sgfx_clear(uint32_t rgb);          // fill the whole surface
void     sgfx_putpixel(int x, int y, uint32_t rgb);
uint32_t sgfx_getpixel(int x, int y);       // 0xRRGGBB at (x,y), 0 if off-screen
void     sgfx_line(int x1, int y1, int x2, int y2, uint32_t rgb);
void     sgfx_fillrect(int x1, int y1, int x2, int y2, uint32_t rgb);
void     sgfx_circle(int cx, int cy, int r, uint32_t rgb);        // outline
void     sgfx_fillcircle(int cx, int cy, int r, uint32_t rgb);
void     sgfx_ellipse(int cx, int cy, int rx, int ry, uint32_t rgb);  // outline
void     sgfx_fillellipse(int cx, int cy, int rx, int ry, uint32_t rgb);
void     sgfx_fillpoly(const int *xy, int npts, uint32_t rgb);    // xy = x0,y0,x1,y1,...
void     sgfx_flood(int x, int y, uint32_t rgb);                  // fill region under (x,y)
void     sgfx_clip(int x1, int y1, int x2, int y2);               // restrict to a rectangle
void     sgfx_noclip(void);                                       // remove the clip
void     sgfx_text(int x, int y, const char *s, int len, uint32_t rgb);  // TTF, baseline (x,y)

#endif
