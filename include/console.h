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

unsigned long long con_micros(void);   // microseconds since boot (for TIME)
int  con_inkey(int centiseconds);      // wait up to n centiseconds for a key; -1 if none
int  con_pos(void);                    // text cursor column (POS)
int  con_vpos(void);                   // text cursor row (VPOS)
// Show the boot logo + banner if appropriate (target QEMU only). Returns 1 if it
// printed `banner` itself (beside the logo); 0 if the caller should print it.
int  con_splash(const char *banner);

// --- BBC-style graphics -----------------------------------------------------
// Coordinates are BBC logical units: x in 0..1279, y in 0..1023, origin at the
// bottom-left, independent of the physical screen resolution.
void con_mode(int n);                    // MODE n: clear text+graphics, reset state
void con_gcol(int action, int colour);   // GCOL action,colour: graphics fg + plot op
void con_plot(int code, int x, int y);   // PLOT code,x,y (master graphics primitive)
void con_clg(void);                      // CLG: clear graphics area to gfx background
int  con_point(int x, int y);            // POINT(x,y): logical colour 0..7, or -1 off-screen

#endif
