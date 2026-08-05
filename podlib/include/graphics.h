#ifndef POD_GRAPHICS_H
#define POD_GRAPHICS_H
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
//   #include <pod.h>
//   #include "graphics.h"
//   int pod_main(const BerryServices *svc,int argc,const char*const*argv){
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

// --- line style: width, joins and caps for thick strokes -------------------
// The pen is in device pixels. `line` honours width + cap; `rectangle` and
// `drawpoly` honour width + join; `circle`/`ellipse` outlines honour width. The
// classic BGI setlinestyle is supported for width only (linestyle/pattern are
// accepted but drawing is always solid). NORM_WIDTH / THICK_WIDTH are the two
// classic widths; setlinewidth takes any width.
enum { SOLID_LINE = 0, DOTTED_LINE, CENTER_LINE, DASHED_LINE, USERBIT_LINE };
enum { NORM_WIDTH = 1, THICK_WIDTH = 3 };
enum { JOIN_MITER = 0, JOIN_BEVEL = 1, JOIN_ROUND = 2 };   // for setlinejoin
enum { CAP_BUTT = 0, CAP_ROUND = 1, CAP_SQUARE = 2 };      // for setlinecap
void setlinestyle(int linestyle, unsigned upattern, int thickness);  // classic (width only)
void setlinewidth(int width);        // pen width in pixels (1 = hairline)
void setlinejoin(int join);          // JOIN_MITER / JOIN_BEVEL / JOIN_ROUND
void setlinecap(int cap);            // CAP_BUTT / CAP_ROUND / CAP_SQUARE

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

// --- graphics mode and coordinate conversion --------------------------------
// Drawing through this library is ALWAYS device pixels, origin top-left, y down -
// whatever coordinate mode BASIC is in. What the mode changes is how coordinates
// a BASIC program *passes to your seed* should be read: in MODE 2 they are the
// same device pixels you draw in (an identity), in MODE 1 they are BBC logical
// units (1280x1024, origin bottom-left) and must be converted.
#define GFX_BBC_W 1280           // the BBC logical extent MODE 1 coordinates use
#define GFX_BBC_H 1024

int  gfx_mode(void);             // BASIC's graphics mode: 1 = BBC logical, 2 = native pixels

// Convert a coordinate pair as BASIC meant it (in the current mode) into the
// device pixels the gfx_* / BGI calls draw in. The inverse hands device pixels
// back as the coordinates BASIC is using. In MODE 2 both are the identity.
void gfx_from_basic(int bx, int by, int *px, int *py);
void gfx_to_basic(int px, int py, int *bx, int *by);

// --- small conveniences ----------------------------------------------------
void gdelay(int centiseconds);           // busy-wait using the seed clock

#endif // POD_GRAPHICS_H
