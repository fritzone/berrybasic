#include "font.h"
#include "graphics.h"

extern unsigned char bbc_font[2048];

// Render one 8x8 BBC glyph at (x,y), scaled by FONT_SCALE.
void draw_char(char c, int x, int y, uint32_t color) {
    unsigned char *glyph = &bbc_font[(unsigned char)c * GLYPH_H];
    for (int row = 0; row < GLYPH_H; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < GLYPH_W; col++) {
            if (bits & (0x80 >> col)) {
                int px = x + col * FONT_SCALE;
                int py = y + row * FONT_SCALE;
                if (FONT_SCALE == 1)
                    putpixel(px, py, color);
                else
                    bar(px, py, px + FONT_SCALE - 1, py + FONT_SCALE - 1, color);
            }
        }
    }
}
