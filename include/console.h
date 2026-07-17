#ifndef CONSOLE_H
#define CONSOLE_H

// Minimal console abstraction that the BASIC interpreter talks to. Two backends
// implement it: the bare-metal framebuffer terminal (kernel.c) and a host stdio
// backend (host/console_host.c) used for testing the interpreter on Linux.

void con_putc(char c);
void con_puts(const char *s);

// Feed one byte to the VDU driver (the BBC VDU statement). Control codes 0..31
// and 127 act on the screen (consuming parameter bytes as needed); codes >= 32
// are printable characters.
void con_vdu(int b);

// Read one line (without trailing newline) into buf, at most maxlen-1 chars.
// Returns the length, or -1 on end-of-input (host only; the target never EOFs).
int  con_getline(char *buf, int maxlen);

// Like con_getline, but prints `prompt` first, starts with `prefill_len`
// editable characters already in buf, and supports in-line cursor editing and
// an up/down command history. Used by the REPL (so AUTO/EDIT can pre-fill a line
// and the arrow keys recall/edit previous input).
int  con_getline_ed(char *buf, int maxlen, int prefill_len, const char *prompt);

void con_cls(void);             // clear screen, cursor home
void con_colour(int c);         // set text foreground colour (BBC COLOUR 0..7)
int  con_getkey(void);          // block for one keypress, return its ASCII code

// Keyboard layout (KEYBOARD statement / KEYBOARD$ function). con_set_keyboard
// selects a layout by two-letter code (US/UK/NO/DK/SE/DE, case-insensitive) and
// returns 1 on success, 0 if unknown. con_get_keyboard returns the active code.
int         con_set_keyboard(const char *code);
const char *con_get_keyboard(void);

unsigned long long con_micros(void);   // microseconds since boot (for TIME)
int  con_inkey(int centiseconds);      // wait up to n centiseconds for a key; -1 if none
int  con_pos(void);                    // text cursor column (POS)
int  con_vpos(void);                   // text cursor row (VPOS)
int  con_rows(void);                   // text rows on screen, or 0 if unpaged (host/tests)
// Show the boot logo + banner if appropriate (target QEMU only). Returns 1 if it
// printed `banner` itself (beside the logo); 0 if the caller should print it.
int  con_splash(const char *banner);

// --- BBC-style graphics -----------------------------------------------------
// Coordinates are BBC logical units: x in 0..1279, y in 0..1023, origin at the
// bottom-left, independent of the physical screen resolution.
void con_mode(int n);                    // MODE n: clear text+graphics, reset state
// SCREEN width,height: switch the physical display resolution for the duration of
// a program. A non-positive w or h restores the startup resolution. Returns 1 on
// success, 0 on failure (or on backends without a real framebuffer). con_screen_w
// / con_screen_h report the current physical size (SCREENW / SCREENH).
int  con_screen(int w, int h);
int  con_screen_w(void);
int  con_screen_h(void);
void con_gcol(int action, int colour);   // GCOL action,colour: graphics fg + plot op
void con_plot(int code, int x, int y);   // PLOT code,x,y (master graphics primitive)
void con_clg(void);                      // CLG: clear graphics area to gfx background
int  con_point(int x, int y);            // POINT(x,y): logical colour 0..7, or -1 off-screen

// --- double buffering -------------------------------------------------------
// con_backbuffer(1) allocates (once) a full-screen off-screen buffer and routes
// all drawing to it; the visible screen freezes until con_flip(). con_backbuffer
// (0) routes drawing back to the visible screen. con_flip() presents the back
// buffer (a no-op when buffering is off). con_buffered() reports the state.
// con_backbuffer returns 0 on success, <0 if the buffer could not be allocated.
int  con_backbuffer(int on);
void con_flip(void);
int  con_buffered(void);

// --- render-to-sprite -------------------------------------------------------
// con_newsprite initialises a DIM buffer as a w*h fully transparent sprite.
// con_target_sprite redirects all drawing into that sprite (coordinates become
// its own pixels, origin bottom-left); returns 0 on success, <0 on a bad sprite.
// con_target_screen returns drawing to the screen (or the back buffer).
void con_newsprite(long addr, int w, int h);
int  con_target_sprite(long addr);
void con_target_screen(void);

// TILEMAP: draw the visible window of a tile grid. `map` -> cols*rows LE u32
// words, row-major (top row first); value 0 = empty, k>=1 = sheet tile (k-1).
// Tiles are tilew x tileh, packed row-major in the `sheet` sprite; scrollx/
// scrolly shift the world. Respects the viewport, target buffer and tint.
void con_tilemap(long sheet, long map, int cols, int rows,
                 int tilew, int tileh, int scrollx, int scrolly);

// --- graphics library (all in BBC logical coordinates) ----------------------
void con_gcol_rgb(int r, int g, int b);              // GCOL r,g,b : truecolour foreground
void con_palette(int logical, int r, int g, int b);  // COLOUR l,r,g,b : redefine a palette slot
void con_line(int x1, int y1, int x2, int y2);       // LINE
void con_rectangle(int x, int y, int w, int h, int fill);  // RECTANGLE [FILL] x,y,w,h
void con_circle(int x, int y, int r, int fill);      // CIRCLE [FILL] x,y,r
void con_ellipse(int x, int y, int rx, int ry, int fill);  // ELLIPSE [FILL] x,y,rx,ry
void con_fill(int x, int y);                         // FILL x,y : flood fill
// Sprites: capture a screen rectangle into a DIM buffer at `addr`, and stamp it
// back (top-left at the given logical point, honouring the plot op).
void con_sprite_get(long addr, int x1, int y1, int x2, int y2);
void con_sprite_put(long addr, int x, int y);
// Blit a sprite scaled by `scale` (1.0 = original) and rotated `angle` degrees
// clockwise about its centre; the centre lands where con_sprite_put's would, so
// (1,0) == con_sprite_put. Honours alpha, GCOL action, viewport, tint, and the
// current draw target. scale<=0 draws nothing; angle is taken mod 360.
void con_sprite_put_ex(long addr, int x, int y, double scale, double angle);
// GTINT: set (on!=0) or clear the sprite tint - out = lerp(src,(r,g,b),a/255),
// applied to every blitted sprite (both con_sprite_put forms), alpha preserved.
void con_sprite_tint(int on, int r, int g, int b, int a);

// --- TrueType text ----------------------------------------------------------
// GTEXT x,y,string$ : draw `s` (len bytes) with the current TrueType font, size
// and style (set via ttf.h / LOADFONT / FONTSIZE / FONTSTYLE) in the current
// graphics foreground colour. (x,y) is the logical coordinate of the baseline at
// the start of the text; glyphs are anti-aliased and honour the viewport, plot
// op, tint and draw target, exactly like a sprite blit. A no-op on backends with
// no framebuffer, or when no font is loaded.
void con_gtext(int x, int y, const char *s, int len);

// --- keyboard modifiers and locks -------------------------------------------
// Which modifier keys are held and which locks are set, as a KMOD_* bitmask
// (see drivers/usb_hid.h): SHIFT CTRL ALT ALTGR META, and CAPS NUM SCROLL.
// These cannot come back from con_getkey, because Shift on its own is not a key
// that types anything - it is state, and this is how you read it.
//
// It is a snapshot taken when the keyboard was last read, so poll a key first
// (con_inkey(0) will do) or the answer is as old as your last read. 0 when no
// USB keyboard is present (a serial terminal sends characters, not modifiers).
int  con_keymods(void);

// --- pointer (USB mouse) ----------------------------------------------------
// Poll the mouse and report the current pointer position in raw framebuffer
// pixels (origin top-left: x in 0..width-1, y in 0..height-1) and the button
// bitmask (bit0=left, bit1=right, bit2=middle). When no mouse is present, or on
// the host backend, position and buttons read back as 0.
void con_mouse(int *x, int *y, int *buttons);

#endif
