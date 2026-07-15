#ifndef TTF_H
#define TTF_H

// TrueType font rendering for BASIC, via stb_truetype (third_party/stb_truetype.h).
//
// Fonts are loaded from storage (LOADFONT) into a persistent pool and referenced
// by small integer handles (1..N); ttf_reset() empties the pool when a program is
// cleared or RUN. One font is "current" at a time, carrying a pixel size and a
// bold/italic/underline style. Text metrics (ttf_text_width / ttf_line_height)
// and glyph rasterization (ttf_glyph, used by the framebuffer backend to draw
// GTEXT) all apply to the current font.
//
// This module owns everything that touches stb_truetype; it exposes no stb types.
// The actual pixels are put on screen by the console backend (con_gtext), which
// asks ttf_glyph for an 8-bit coverage bitmap and blits it in the graphics
// foreground colour. On the host backend the metrics work (real stb + libc) but
// there is no framebuffer, so con_gtext is a no-op.

// Load a .ttf/.ttc file. Returns a handle > 0, or 0 on failure (missing file,
// unreadable, not a font, or the pool is full). A successful load also makes the
// new font the current one.
int  ttf_load(const char *filename);

// Make an already-loaded handle the current font. Returns 1 on success, 0 if the
// handle is unknown.
int  ttf_select(int handle);

void ttf_set_size(int pixels);      // FONTSIZE: glyph cell height in pixels (min 1)
void ttf_set_style(int bold, int italic, int underline);   // FONTSTYLE flags (0/1)
void ttf_reset(void);               // drop all loaded fonts (called on RUN / NEW)

int  ttf_ready(void);               // 1 if a font is current, else 0
int  ttf_line_height(void);         // ascent+descent (+gap) in pixels at the size
int  ttf_ascent(void);              // pixels from the baseline up to the cell top
int  ttf_text_width(const char *s, int len);   // advance width of s, in pixels
int  ttf_style_flags(void);         // bit0 = bold, bit1 = italic, bit2 = underline

// Rasterize one Unicode code point into an internal 8-bit coverage bitmap
// (0 = transparent .. 255 = solid), valid until the next ttf_glyph call. Fills:
//   *w,*h        bitmap size in pixels (0x0 for a blank glyph such as space)
//   *xoff,*yoff  top-left of the bitmap relative to the pen on the baseline
//                (yoff is normally negative: the glyph rises above the baseline)
//   *advance     how far to move the pen after this glyph, in pixels
// Returns 1 on success (a blank glyph is still success, with *bitmap possibly 0),
// or 0 if no font is current.
int  ttf_glyph(int codepoint, const unsigned char **bitmap,
               int *w, int *h, int *xoff, int *yoff, int *advance);

#endif
