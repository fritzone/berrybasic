#ifndef INTERP_GRAPHICS_H
#define INTERP_GRAPHICS_H

#include "interp_types.h"

/* ==================================================================
 * interp_graphics.c -- Graphics statements plus assorted misc statements.
 * ================================================================== */

/* ------------------------------------------------------------ functions */

// Execute the SCREEN statement.
void stmt_screen (void);

// KEYBOARD "NO" : select the keyboard layout by two-letter code (US/UK/NO/DK/SE/
// DE, case-insensitive). Affects how physical key presses are decoded from here
// on; the current layout can be read back with the KEYBOARD$ function.
void stmt_keyboard (void);

// POKE addr, byte : store one byte at a memory address. Familiar alias for the
// indirection form `?addr = byte`; use `!addr = word` / `$addr = "..."` for a
// 32-bit word or a string. On this bare-metal machine the address is real, so
// poke inside a DIMed buffer (see DIM name size) unless you mean a hardware one.
void stmt_poke_kw (void);

// EXEC "statement" : run a string as if it were a line of BASIC, in the current
// context (same variables, arrays, open loops). The companion to EVAL: EVAL
// computes a value, EXEC performs actions. Build the string however you like —
// a config file line, an assembled "LET " + n$ + " = " + v$, a typed command.
// The lexer is pointed at a private copy of the string and restored afterwards,
// so the statement that issued EXEC carries on normally (e.g. EXEC c$ : PRINT).
// A branch (GOTO/GOSUB) or END inside the string propagates out as usual; a
// half-open block (a FOR with no NEXT, say) is a mistake, because the copied
// text is gone once EXEC returns.
void stmt_exec (void);

// Execute the MODE statement.
void stmt_mode (void);

// LINEWIDTH n : set the pen width (coordinate units) for thick lines and outlines.
void stmt_linewidth (void);

// LINEJOIN MITER | BEVEL | ROUND : how the corners of a stroked outline are drawn.
void stmt_linejoin (void);

// LINECAP BUTT | ROUND | SQUARE : how the open ends of a stroked line are drawn.
void stmt_linecap (void);

// A packed colour from RGB() carries bit 30, so it can't be mistaken for a
// logical colour index (0..15 / 128..143).
void gcol_apply_packed (int packed);

// GCOL colour            (action 0)
// GCOL action, colour    (colour = logical index, or a packed RGB() value)
// GCOL r, g, b           (24-bit truecolour foreground)
void stmt_gcol (void);

// Read `n` more comma-separated numbers into out[]. Returns 0 on error.
int read_nums (int *out, int n);

// LINE x1,y1,x2,y2
void stmt_line (void);

// After a shape keyword, an optional FILL modifier means the solid variant.
int shape_fill (void);

// RECTANGLE [FILL] x,y,w,h
void stmt_rectangle (void);

// CIRCLE [FILL] x,y,r
void stmt_circle (void);

// ELLIPSE [FILL] x,y,rx,ry
void stmt_ellipse (void);

// FILL x,y : flood fill from a point
void stmt_fill (void);

// GGET addr, x1,y1,x2,y2 : capture a screen rectangle into a DIM buffer
void stmt_gget (void);

// GPUT addr,x,y [,scale,angle] : stamp a sprite, optionally scaled and rotated.
void stmt_gput (void);

// GTINT r,g,b,a | GTINT OFF : tint every subsequently blitted sprite.
void stmt_gtint (void);

// NEWSPRITE addr, w, h : initialise a DIM buffer as a blank transparent sprite.
void stmt_newsprite (void);

// SPRITETARGET addr | SPRITETARGET OFF : redirect drawing into a sprite (or back).
void stmt_spritetarget (void);

// TILEMAP sheet,map,cols,rows,tilew,tileh,scrollx,scrolly : draw a tile grid.
void stmt_tilemap (void);

// FONT handle : make a font loaded with LOADFONT the current one.
void stmt_font (void);

// FONTSIZE pixels : set the current font's glyph height.
void stmt_fontsize (void);

// FONTSTYLE bold [, italic [, underline]] : each a 0/1 flag (default 0).
void stmt_fontstyle (void);

// GTEXT x, y, s$ : draw text with the current font at logical baseline (x,y).
void stmt_gtext (void);

// Copy a just-read string key into a stable local buffer: the value expression
// that follows may run the GC and move the key's bytes in the string heap.
int grab_key (value_t k, char *buf);

// DICTSET d, key$, value   |   DICTDEL d, key$
void stmt_dictset (void);

// Execute the DICTDEL statement.
void stmt_dictdel (void);

// PUSH L, value  |  LISTSET L, i, value  |  LISTINS L, i, value  |  LISTDEL L, i
void stmt_push (void);

// Execute the LISTSET statement.
void stmt_listset (void);

// Execute the LISTINS statement.
void stmt_listins (void);

// Execute the LISTDEL statement.
void stmt_listdel (void);

// TREESET t, key, value   |   TREEDEL t, key
void stmt_treeset (void);

// Execute the TREEDEL statement.
void stmt_treedel (void);

// BUFFER ON | BUFFER OFF : enable/disable off-screen drawing (double buffering).
// While on, every draw targets the back buffer and the screen freezes until FLIP.
void stmt_buffer (void);

// FLIP : present the back buffer to the screen (a no-op when buffering is off).
void stmt_flip (void);

// SAVESPRITE addr, "file" : write a sprite (LOADSPRITE result or GGET capture)
// out as an image file (PNG, or BMP if the name ends in .bmp).
void stmt_savesprite (void);

// PLOT code,x,y
void stmt_plot (void);

// MOVE x,y  ==  PLOT 4,x,y     DRAW x,y  ==  PLOT 5,x,y
void stmt_move_draw (int plotcode);

#endif /* INTERP_GRAPHICS_H */
