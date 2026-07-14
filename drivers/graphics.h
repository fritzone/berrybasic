#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdint.h>

#define COLOR_BLACK   0xFF000000
#define COLOR_WHITE   0xFFFFFFFF
#define COLOR_RED     0xFFFF0000
#define COLOR_GREEN   0xFF00FF00
#define COLOR_BLUE    0xFF0000FF
#define COLOR_YELLOW  0xFFFFFF00
#define COLOR_CYAN    0xFF00FFFF
#define COLOR_MAGENTA 0xFFFF00FF
#define COLOR_GRAY    0xFF808080

typedef struct { int x1, y1, x2, y2, clip; } viewport_t;

void init_graphics(uint32_t *fb, uint32_t w, uint32_t h, uint32_t pitch);

// --- double buffering -------------------------------------------------------
// gfx_backbuffer(1) redirects every primitive to an off-screen back buffer
// (allocated once, sized to the current front buffer); gfx_backbuffer(0)
// restores drawing to the visible framebuffer. gfx_flip() copies the back
// buffer onto the front. gfx_buffered() reports whether drawing is buffered.
// Returns 0 on success, <0 if the resolution exceeds the back-buffer capacity.
int  gfx_backbuffer(int on);
void gfx_flip(void);
int  gfx_buffered(void);

// Render-to-sprite: gfx_set_target points every primitive at an off-screen WxH
// surface (tightly packed, pitch = w); gfx_reset_target restores the screen
// (front or back buffer). Used by SPRITETARGET.
void gfx_set_target(uint32_t *buf, uint32_t w, uint32_t h);
void gfx_reset_target(void);

void putpixel(int x, int y, uint32_t color);
void putpixel_op(int x, int y, uint32_t color);   // like putpixel but applies the plot op
uint32_t getpixel(int x, int y);
void bar(int x1, int y1, int x2, int y2, uint32_t color);
void cleardevice(void);

// --- BBC-style graphics primitives (honour the current plot op, see gfx_set_op).
// These operate in physical framebuffer pixels.
void gfx_set_op(int op);   // 0=store(copy) 1=OR 2=AND 3=EOR 4=invert(NOT, colour ignored)

// Graphics viewport clip rectangle (physical pixels). gfx_set_clip restricts all
// drawing; gfx_clear_clip removes the restriction; gfx_clip_rect reports the
// current bounds (full screen when no clip is set).
void gfx_set_clip(int x0, int y0, int x1, int y1);
void gfx_clear_clip(void);
void gfx_clip_rect(int *x0, int *y0, int *x1, int *y1);

void draw_glyph_op(const unsigned char *glyph, int x, int y, int scale, uint32_t color);
void draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void fill_rect(int x0, int y0, int x1, int y1, uint32_t color);
void draw_rect(int x0, int y0, int x1, int y1, uint32_t color);   // outline
void fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color);
void draw_circle(int cx, int cy, int r, uint32_t color);
void fill_circle(int cx, int cy, int r, uint32_t color);
void draw_ellipse(int cx, int cy, int rx, int ry, uint32_t color); // outline
void fill_ellipse(int cx, int cy, int rx, int ry, uint32_t color);
// Flood-fill the connected region of one colour starting at (x,y) with `color`
// (honours the clip rectangle; writes solid pixels regardless of plot op).
void flood_fill(int x, int y, uint32_t color);

// Shift the whole framebuffer up by `pixels` rows; fill the exposed bottom
// `pixels` rows with `fill`. Used by the text console to scroll.
void scroll_up(int pixels, uint32_t fill);
void scroll_rect(int x0, int y0, int x1, int y1, int pixels, uint32_t fill);
void move_rect(int x0, int y0, int x1, int y1, int dx, int dy, uint32_t fill);

#endif
