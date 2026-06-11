#ifndef FONT_H
#define FONT_H

#include <stdint.h>

// The BBC Micro character set is 8x8. We render each glyph at an integer scale
// for a chunky, period-accurate look (FONT_SCALE=1 gives the raw 8x8 font).
#define GLYPH_W     8
#define GLYPH_H     8
#define FONT_SCALE  2
#define CHAR_W      (GLYPH_W * FONT_SCALE)
#define CHAR_H      (GLYPH_H * FONT_SCALE)

void draw_char(char c, int x, int y, uint32_t color);

#endif
