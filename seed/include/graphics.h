#ifndef SEED_GRAPHICS_H
#define SEED_GRAPHICS_H
// ---------------------------------------------------------------------------
// Basic Graphics Interface library for native seeds, familiar names — line/circle/rectangle/
// bar/arc/pieslice/floodfill, setcolor/setfillstyle, outtextxy — draw straight
// onto the BerryBasiC framebuffer through the seed services (ABI v7).
//
// Differences from the DOS original, all in the modern direction:
//   * Colours are 24-bit truecolour. The classic 16 BGI colour NAMES still work
//     as a palette (setcolor(RED)); setrgbcolor(r,g,b) unlocks the full range.
//   * Text is rendered with TrueType fonts (loadfont/settextsize) instead of the
//     old bitmap .CHR stroke fonts, so it is smooth and scalable.
//   * initgraph() takes no driver/mode — the screen is already up; it just
//     reports whether a framebuffer is present (0 on the host build).
//
// Coordinates are device pixels, origin TOP-LEFT (x right, y down) — the BGI
// convention, and the opposite of BASIC's logical bottom-left graphics.
//
//   #include "seed.h"
//   #include "graphics.h"
//   SEED_EXPORT(demo) {
//       if (!initgraph()) return 0;
//       setcolor(YELLOW); circle(getmaxx()/2, getmaxy()/2, 100);
//       return 1;
//   }
// ---------------------------------------------------------------------------

// The sixteen standard BGI colours (indices for setcolor/setfillstyle/putpixel).
enum {
    BLACK = 0, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LIGHTGRAY,
    DARKGRAY, LIGHTBLUE, LIGHTGREEN, LIGHTCYAN, LIGHTRED, LIGHTMAGENTA,
    YELLOW, WHITE
};

// Fill patterns for setfillstyle. Only SOLID_FILL and EMPTY_FILL change the
// result today (others are accepted and treated as solid).
enum {
    EMPTY_FILL = 0, SOLID_FILL, LINE_FILL, LTSLASH_FILL, SLASH_FILL,
    BKSLASH_FILL, LTBKSLASH_FILL, HATCH_FILL, XHATCH_FILL, INTERLEAVE_FILL,
    WIDE_DOT_FILL, CLOSE_DOT_FILL, USER_FILL
};

// Text justification (settextjustify).
enum { LEFT_TEXT = 0, CENTER_TEXT = 1, RIGHT_TEXT = 2 };
enum { BOTTOM_TEXT = 0, VCENTER_TEXT = 1, TOP_TEXT = 2 };

// --- setup / info ----------------------------------------------------------
int  initgraph(void);       // 1 if a framebuffer is present (0 on the host build)
void closegraph(void);      // release fonts, restore defaults
int  getmaxx(void);         // rightmost pixel (width - 1)
int  getmaxy(void);         // bottommost pixel (height - 1)
int  getmaxcolor(void);     // highest palette index (15)
void cleardevice(void);     // clear the whole screen to the background colour

// --- colour ----------------------------------------------------------------
unsigned int rgb(int r, int g, int b);   // pack a 0xRRGGBB truecolour value
void setcolor(int color);                // drawing colour, palette index 0..15
void setrgbcolor(int r, int g, int b);   // drawing colour, truecolour
int  getcolor(void);                     // current drawing colour as 0xRRGGBB
void setbkcolor(int color);              // background (used by cleardevice)
void setrgbbkcolor(int r, int g, int b);
int  getbkcolor(void);
void setfillstyle(int pattern, int color);   // fill colour, palette index
void setrgbfillcolor(int r, int g, int b);

// --- pixels and the current position ---------------------------------------
void putpixel(int x, int y, int color);  // plot one pixel, palette index
int  getpixel(int x, int y);             // read a pixel as 0xRRGGBB
void moveto(int x, int y);
void moverel(int dx, int dy);
void lineto(int x, int y);               // line from CP to (x,y); CP moves there
void linerel(int dx, int dy);
int  getx(void);
int  gety(void);

// --- shapes (outline uses the drawing colour, fills use the fill colour) ----
void line(int x1, int y1, int x2, int y2);
void rectangle(int left, int top, int right, int bottom);
void bar(int left, int top, int right, int bottom);
void bar3d(int left, int top, int right, int bottom, int depth, int topflag);
void circle(int x, int y, int radius);
void arc(int x, int y, int stangle, int endangle, int radius);
void ellipse(int x, int y, int stangle, int endangle, int xradius, int yradius);
void fillellipse(int x, int y, int xradius, int yradius);
void pieslice(int x, int y, int stangle, int endangle, int radius);
void sector(int x, int y, int stangle, int endangle, int xradius, int yradius);
void drawpoly(int numpoints, const int *polypoints);
void fillpoly(int numpoints, const int *polypoints);
void floodfill(int x, int y, int border);
void setviewport(int x1, int y1, int x2, int y2, int clip);
void clearviewport(void);

// --- text: TrueType fonts --------------------------------------------------
int  loadfont(const char *filename);     // load a .ttf -> handle, 0 on failure
void settextfont(int handle);            // select a loaded font
void settextsize(int pixels);            // glyph height in pixels
void settextstyle(int font, int direction, int charsize);  // font=handle, dir ignored
void setfontstyle(int bold, int italic, int underline);    // 0/1 flags
void settextjustify(int horiz, int vert);
void outtext(const char *s);             // draw at the current position
void outtextxy(int x, int y, const char *s);
int  textwidth(const char *s);
int  textheight(const char *s);

// --- double buffering ------------------------------------------------------
// Draw the next frame off-screen, then show it in one go, so an animating or
// redrawing loop never displays a half-drawn screen. The off-screen buffer
// keeps its contents between flips, so a loop may redraw only what changed.
//
//   setdoublebuffer(1);
//   for (;;) { ...draw...; flippage(); }      // each frame appears complete
//   setdoublebuffer(0);                       // back to drawing on the screen
//
// This is the same buffer, and the same on/off setting, as BASIC's BUFFER ON /
// FLIP: if you turn it on, put it back the way you found it before returning
// (getdoublebuffer() reports the current setting) so you don't break a program
// that was buffering its own graphics.
int  setdoublebuffer(int on);   // 1 = draw off-screen; 0 = straight to the screen.
                                //   Returns 0 on success, <0 if no buffer could
                                //   be had (drawing then just goes to the screen).
void flippage(void);            // present the finished frame (no-op when off)
int  getdoublebuffer(void);     // 1 if drawing is currently going off-screen

// --- small conveniences ----------------------------------------------------
void gdelay(int centiseconds);           // busy-wait using the seed clock

#endif // SEED_GRAPHICS_H
