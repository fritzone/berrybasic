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
void putpixel(int x, int y, uint32_t color);
uint32_t getpixel(int x, int y);
void bar(int x1, int y1, int x2, int y2, uint32_t color);
void cleardevice(void);

// --- BBC-style graphics primitives (honour the current plot op, see gfx_set_op).
// These operate in physical framebuffer pixels.
void gfx_set_op(int op);   // 0=store(copy) 1=OR 2=AND 3=EOR 4=invert(NOT, colour ignored)
void draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void fill_rect(int x0, int y0, int x1, int y1, uint32_t color);
void fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color);
void draw_circle(int cx, int cy, int r, uint32_t color);
void fill_circle(int cx, int cy, int r, uint32_t color);

// Shift the whole framebuffer up by `pixels` rows; fill the exposed bottom
// `pixels` rows with `fill`. Used by the text console to scroll.
void scroll_up(int pixels, uint32_t fill);

#endif
