// BGI-style graphics library for PODs (see seed/include/graphics.h).
// A thin, familiar layer over the ABI v7 graphics services: it keeps the classic
// BGI state (current colour, fill colour, current position, text justification)
// on the seed side and turns the high-level calls (rectangle, arc, pieslice,
// outtextxy, ...) into the primitive gfx_* service calls. --gc-sections drops all
// of this from a seed that never draws.
#include "pod_rt.h"
#include "graphics.h"

#ifndef BERRY_SEED   /* the capability manifest is a POD concept; seeds get ungated services */
POD_NEEDS(CAP_GRAPHICS | CAP_TIME, "GRAPHICS=drawing and text; TIME=gdelay")
#endif

// The sixteen classic BGI/EGA colours as 0xRRGGBB.
static const unsigned int g_pal[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

static unsigned int cur_color  = 0xFFFFFF;   // drawing colour (outline / lines / text)
static unsigned int bk_color   = 0x000000;   // background (cleardevice)
static unsigned int fill_color = 0xFFFFFF;   // fill colour (bar / pieslice / fillpoly)
static int fill_pat = SOLID_FILL;
static int cp_x = 0, cp_y = 0;               // current position (moveto/lineto)
static int just_h = LEFT_TEXT, just_v = TOP_TEXT;
static int s_lw = 1, s_lj = JOIN_MITER, s_lc = CAP_BUTT;   // line pen (see setlinewidth)
static void push_line_style(void);

static int  slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static unsigned int pal(int c)  { return g_pal[c & 15]; }

// --- degrees sin/cos (no libm in a seed): range-reduce + short Taylor series --
#define BGI_PI 3.14159265358979323846
static double s_sin(double x) {
    double twopi = 2 * BGI_PI, halfpi = BGI_PI / 2;
    double kq = x / twopi; kq = (double)(long)(kq >= 0 ? kq + 0.5 : kq - 0.5); x -= kq * twopi;
    double nq = x / halfpi; int n = (int)(nq >= 0 ? nq + 0.5 : nq - 0.5);
    double r = x - n * halfpi, r2 = r * r, st = r, ss = r;
    for (int i = 1; i < 8; i++) { st *= -r2 / ((2 * i) * (2 * i + 1)); ss += st; }
    double ct = 1, cc = 1;
    for (int i = 1; i < 8; i++) { ct *= -r2 / ((2 * i - 1) * (2 * i)); cc += ct; }
    switch (((n % 4) + 4) % 4) { case 0: return ss; case 1: return cc; case 2: return -ss; default: return -cc; }
}
// BGI angles: 0 deg = east, increasing counter-clockwise (so +y is drawn upward,
// against the screen's downward y).
static double dcos_deg(double d) { return s_sin((d + 90.0) * BGI_PI / 180.0); }
static double dsin_deg(double d) { return s_sin(d * BGI_PI / 180.0); }

// --- setup / info ----------------------------------------------------------
int initgraph(void) {
    cur_color = 0xFFFFFF; bk_color = 0x000000; fill_color = 0xFFFFFF;
    fill_pat = SOLID_FILL; cp_x = cp_y = 0; just_h = LEFT_TEXT; just_v = TOP_TEXT;
    s_lw = 1; s_lj = JOIN_MITER; s_lc = CAP_BUTT; push_line_style();   // default pen
    return berry_svc->gfx_avail();
}
void closegraph(void) { berry_svc->gfx_noclip(); }
int  getmaxx(void) { int w = berry_svc->gfx_width();  return w > 0 ? w - 1 : 0; }
int  getmaxy(void) { int h = berry_svc->gfx_height(); return h > 0 ? h - 1 : 0; }
int  getmaxcolor(void) { return 15; }
void cleardevice(void) { berry_svc->gfx_noclip(); berry_svc->gfx_clear(bk_color); }

// --- double buffering ------------------------------------------------------
int  setdoublebuffer(int on) { return berry_svc->gfx_backbuffer(on); }
void flippage(void)          { berry_svc->gfx_flip(); }
int  getdoublebuffer(void)   { return berry_svc->gfx_buffered(); }

// --- colour ----------------------------------------------------------------
unsigned int rgb(int r, int g, int b) { return ((r & 255) << 16) | ((g & 255) << 8) | (b & 255); }
void setcolor(int c)                 { cur_color = pal(c); }
void setrgbcolor(int r, int g, int b){ cur_color = rgb(r, g, b); }
int  getcolor(void)                  { return (int)cur_color; }
void setbkcolor(int c)               { bk_color = pal(c); }
void setrgbbkcolor(int r, int g, int b) { bk_color = rgb(r, g, b); }
int  getbkcolor(void)                { return (int)bk_color; }
void setfillstyle(int pat, int c)    { fill_pat = pat; fill_color = pal(c); }
void setrgbfillcolor(int r, int g, int b) { fill_pat = SOLID_FILL; fill_color = rgb(r, g, b); }

// --- pixels and the current position ---------------------------------------
void putpixel(int x, int y, int c) { berry_svc->gfx_putpixel(x, y, pal(c)); }
int  getpixel(int x, int y)        { return (int)berry_svc->gfx_getpixel(x, y); }
void moveto(int x, int y)          { cp_x = x; cp_y = y; }
void moverel(int dx, int dy)       { cp_x += dx; cp_y += dy; }
void lineto(int x, int y)          { berry_svc->gfx_line(cp_x, cp_y, x, y, cur_color); cp_x = x; cp_y = y; }
void linerel(int dx, int dy)       { lineto(cp_x + dx, cp_y + dy); }
int  getx(void)                    { return cp_x; }
int  gety(void)                    { return cp_y; }

// --- shapes ----------------------------------------------------------------
// --- line style -------------------------------------------------------------
// The pen (width/join/cap) lives in the kernel graphics layer; the seed keeps a
// copy so each setter can push the full triple through the one service.
static void push_line_style(void) { berry_svc->gfx_line_style(s_lw, s_lj, s_lc); }
void setlinewidth(int w)  { s_lw = w < 1 ? 1 : w; push_line_style(); }
void setlinejoin(int j)   { s_lj = j; push_line_style(); }
void setlinecap(int c)    { s_lc = c; push_line_style(); }
void setlinestyle(int linestyle, unsigned upattern, int thickness) {
    (void)linestyle; (void)upattern;            // pattern/dash not supported (solid only)
    setlinewidth(thickness);
}

void line(int x1, int y1, int x2, int y2) { berry_svc->gfx_line(x1, y1, x2, y2, cur_color); }

void rectangle(int l, int t, int r, int b) {
    int pts[8] = { l, t, r, t, r, b, l, b };
    berry_svc->gfx_polyline(pts, 4, 1 /*closed*/, cur_color);   // honours width + join
}
void bar(int l, int t, int r, int b) {
    if (fill_pat != EMPTY_FILL) berry_svc->gfx_fillrect(l, t, r, b, fill_color);
}
void bar3d(int l, int t, int r, int b, int depth, int topflag) {
    if (fill_pat != EMPTY_FILL) berry_svc->gfx_fillrect(l, t, r, b, fill_color);
    rectangle(l, t, r, b);
    if (depth > 0) {
        int dx = depth, dy = -depth;             // BGI depth rises up-and-right
        berry_svc->gfx_line(r, t, r + dx, t + dy, cur_color);
        berry_svc->gfx_line(r, b, r + dx, b + dy, cur_color);
        berry_svc->gfx_line(r + dx, b + dy, r + dx, t + dy, cur_color);
        if (topflag) {
            berry_svc->gfx_line(l, t, l + dx, t + dy, cur_color);
            berry_svc->gfx_line(l + dx, t + dy, r + dx, t + dy, cur_color);
        }
    }
}
void circle(int x, int y, int r) { berry_svc->gfx_circle(x, y, r, cur_color); }

void ellipse(int cx, int cy, int a0, int a1, int rx, int ry) {
    if (a1 < a0) a1 += 360;
    int steps = a1 - a0; if (steps < 1) steps = 1; if (steps > 360) steps = 360;
    int px = 0, py = 0, first = 1;
    for (int i = 0; i <= steps; i++) {
        double a = a0 + (double)(a1 - a0) * i / steps;
        int X = cx + (int)(rx * dcos_deg(a));
        int Y = cy - (int)(ry * dsin_deg(a));
        if (!first) berry_svc->gfx_line(px, py, X, Y, cur_color);
        px = X; py = Y; first = 0;
    }
}
void arc(int x, int y, int a0, int a1, int r) { ellipse(x, y, a0, a1, r, r); }

void fillellipse(int cx, int cy, int rx, int ry) {
    if (fill_pat != EMPTY_FILL) berry_svc->gfx_fillellipse(cx, cy, rx, ry, fill_color);
    berry_svc->gfx_ellipse(cx, cy, rx, ry, cur_color);
}

// A filled elliptical sector (pie wedge): centre + a fan of arc points.
void sector(int cx, int cy, int a0, int a1, int rx, int ry) {
    int pts[2 * 130], n = 0;
    pts[n++] = cx; pts[n++] = cy;
    if (a1 < a0) a1 += 360;
    int steps = a1 - a0; if (steps < 1) steps = 1; if (steps > 128) steps = 128;
    for (int i = 0; i <= steps && n < 2 * 129; i++) {
        double a = a0 + (double)(a1 - a0) * i / steps;
        pts[n++] = cx + (int)(rx * dcos_deg(a));
        pts[n++] = cy - (int)(ry * dsin_deg(a));
    }
    if (fill_pat != EMPTY_FILL) berry_svc->gfx_fillpoly(pts, n / 2, fill_color);
    for (int i = 0; i + 3 < n; i += 2)
        berry_svc->gfx_line(pts[i], pts[i + 1], pts[i + 2], pts[i + 3], cur_color);
    berry_svc->gfx_line(pts[n - 2], pts[n - 1], cx, cy, cur_color);   // close back to centre
    berry_svc->gfx_line(cx, cy, pts[2], pts[3], cur_color);
}
void pieslice(int x, int y, int a0, int a1, int r) { sector(x, y, a0, a1, r, r); }

void drawpoly(int npts, const int *p) {
    berry_svc->gfx_polyline(p, npts, 0 /*open*/, cur_color);   // honours width + join + caps
}
void fillpoly(int npts, const int *p) {
    if (npts < 2) return;
    if (fill_pat != EMPTY_FILL) berry_svc->gfx_fillpoly(p, npts, fill_color);
    for (int i = 0; i + 1 < npts; i++)
        berry_svc->gfx_line(p[i * 2], p[i * 2 + 1], p[(i + 1) * 2], p[(i + 1) * 2 + 1], cur_color);
    berry_svc->gfx_line(p[(npts - 1) * 2], p[(npts - 1) * 2 + 1], p[0], p[1], cur_color);
}
void floodfill(int x, int y, int border) { (void)border; berry_svc->gfx_flood(x, y, fill_color); }

void setviewport(int x1, int y1, int x2, int y2, int clip) {
    if (clip) berry_svc->gfx_clip(x1, y1, x2, y2); else berry_svc->gfx_noclip();
}
void clearviewport(void) { berry_svc->gfx_clear(bk_color); }   // clears the current clip region

// --- text ------------------------------------------------------------------
int  loadfont(const char *f)      { return berry_svc->font_load(f); }
void settextfont(int h)           { berry_svc->font_select(h); }
void settextsize(int px)          { berry_svc->font_size(px); }
void settextstyle(int font, int direction, int charsize) {
    (void)direction;
    if (font > 0)     berry_svc->font_select(font);
    if (charsize > 0) berry_svc->font_size(charsize);
}
void setfontstyle(int b, int i, int u) { berry_svc->font_style(b, i, u); }
void settextjustify(int h, int v)      { just_h = h; just_v = v; }
int  textwidth(const char *s)          { return berry_svc->text_width(s, slen(s)); }
int  textheight(const char *s)         { (void)s; return berry_svc->text_height(); }

// Place text per the current justification. gfx_text wants the baseline; we
// approximate the ascent as 4/5 of the line height for TOP/CENTER/BOTTOM anchors.
static void draw_text_anchored(int x, int y, const char *s) {
    int len = slen(s);
    int w = berry_svc->text_width(s, len);
    int h = berry_svc->text_height();
    int ascent = (h * 4) / 5;
    int bx = x, by = y + ascent;
    if (just_h == CENTER_TEXT) bx -= w / 2; else if (just_h == RIGHT_TEXT) bx -= w;
    if (just_v == VCENTER_TEXT) by -= h / 2; else if (just_v == BOTTOM_TEXT) by -= h;
    berry_svc->gfx_text(bx, by, s, len, cur_color);
}
void outtextxy(int x, int y, const char *s) { draw_text_anchored(x, y, s); }
void outtext(const char *s) {
    draw_text_anchored(cp_x, cp_y, s);
    cp_x += berry_svc->text_width(s, slen(s));
}

// --- conveniences ----------------------------------------------------------
// --- graphics mode and coordinate conversion --------------------------------
int gfx_mode(void) { return berry_svc->gfx_mode(); }

void gfx_from_basic(int bx, int by, int *px, int *py) {
    if (berry_svc->gfx_mode() == 2) { if (px) *px = bx; if (py) *py = by; return; }
    int w = berry_svc->gfx_width(), h = berry_svc->gfx_height();
    if (px) *px = bx * w / GFX_BBC_W;
    if (py) *py = (h - 1) - by * h / GFX_BBC_H;          // BBC y-up -> device y-down
}

void gfx_to_basic(int px, int py, int *bx, int *by) {
    if (berry_svc->gfx_mode() == 2) { if (bx) *bx = px; if (by) *by = py; return; }
    int w = berry_svc->gfx_width(), h = berry_svc->gfx_height();
    if (bx) *bx = px * GFX_BBC_W / w;
    if (by) *by = (h - 1 - py) * GFX_BBC_H / h;
}

void gdelay(int cs) {
    unsigned t = berry_svc->time_cs();
    while ((unsigned)(berry_svc->time_cs() - t) < (unsigned)cs) { }
}
