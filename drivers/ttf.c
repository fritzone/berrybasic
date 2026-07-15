// TrueType font rendering via stb_truetype (bare-metal target backend).
//
// Like drivers/image.c, stb_truetype assumes a hosted libc (a heap and a little
// math). Freestanding, we supply all of it here through the STBTT_* hooks below,
// so stb pulls in no system headers at all:
//   - memory comes from two bump arenas: `font_arena` holds the raw TTF bytes of
//     every loaded font (persistent - stbtt_fontinfo points into it - until
//     ttf_reset), and `scratch` is reset before each glyph render for stb's
//     working buffers and the returned coverage bitmap;
//   - the maths stb needs on the render path (floor/ceil/sqrt/fabs) are tiny
//     local routines; pow/fmod/cos/acos are only used by stb's signed-distance-
//     field helpers, which we never call, so they are harmless stubs.
//
// Coverage bitmaps are 8-bit alpha; the console backend (con_gtext) blits them in
// the current graphics colour, so anti-aliasing, tint and the plot op all come
// for free from the existing sprite pixel path.
#include <stddef.h>
#include <stdint.h>
#include "ttf.h"
#include "storage.h"

// stb needs a few mem*/strlen helpers; mem* live in kernel/util.c, strlen below.
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

// --- arenas -----------------------------------------------------------------
// Persistent store for loaded font files (stbtt_fontinfo indexes into these).
#define FONT_ARENA_SIZE  (8 * 1024 * 1024)
static unsigned char font_arena[FONT_ARENA_SIZE] __attribute__((aligned(16)));
static size_t        font_off;

// Per-glyph scratch: stb's shape/edge buffers plus the coverage bitmap. Reset at
// the top of every ttf_glyph, so one glyph's bitmap stays valid until the next.
#define SCRATCH_SIZE     (8 * 1024 * 1024)
static unsigned char scratch[SCRATCH_SIZE] __attribute__((aligned(16)));
static size_t        scratch_off;

static void  scratch_reset(void) { scratch_off = 0; }
static void *scratch_alloc(size_t sz) {
    size_t start = (scratch_off + 15) & ~(size_t)15;
    if (start + sz > SCRATCH_SIZE) return 0;
    scratch_off = start + sz;
    return scratch + start;
}

// --- tiny maths stb references (freestanding) -------------------------------
static double tt_floor(double x) { double t = (double)(long long)x; return t > x ? t - 1 : t; }
static double tt_ceil(double x)  { double t = (double)(long long)x; return t < x ? t + 1 : t; }
static double tt_fabs(double x)  { return x < 0 ? -x : x; }
static double tt_sqrt(double x) {                 // Newton-Raphson, ample for AA
    if (x <= 0) return 0;
    double r = x > 1 ? x : 1;
    for (int i = 0; i < 40; i++) r = 0.5 * (r + x / r);
    return r;
}
// SDF-only: never reached on our render path, present so stb compiles.
static double tt_stub2(double a, double b) { (void)a; (void)b; return 0; }
static double tt_stub1(double a) { (void)a; return 0; }

static int strlen_ttf(const char *s) { int n = 0; while (s[n]) n++; return n; }

// --- stb_truetype (freestanding configuration) ------------------------------
#define STBTT_ifloor(x)    ((int)tt_floor(x))
#define STBTT_iceil(x)     ((int)tt_ceil(x))
#define STBTT_sqrt(x)      tt_sqrt(x)
#define STBTT_pow(x,y)     tt_stub2(x,y)
#define STBTT_fmod(x,y)    tt_stub2(x,y)
#define STBTT_cos(x)       tt_stub1(x)
#define STBTT_acos(x)      tt_stub1(x)
#define STBTT_fabs(x)      tt_fabs(x)
#define STBTT_malloc(x,u)  ((void)(u), scratch_alloc(x))
#define STBTT_free(x,u)    ((void)(u), (void)(x))
#define STBTT_assert(x)    ((void)0)
#define STBTT_strlen(x)    strlen_ttf(x)
#define STBTT_memcpy       memcpy
#define STBTT_memset       memset
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// --- loaded-font table ------------------------------------------------------
#define MAX_FONTS 8
static struct {
    int             used;
    stbtt_fontinfo  info;
} fonts[MAX_FONTS];

static int    cur = -1;             // index of the current font, or -1
static int    size_px = 24;         // FONTSIZE
static int    st_bold, st_italic, st_underline;

// Cached vertical metrics + scale for the current font/size (recomputed lazily).
static float  cur_scale;
static int    cur_ascent, cur_descent, cur_linegap;

static void recompute_metrics(void) {
    if (cur < 0) { cur_scale = 0; cur_ascent = cur_descent = cur_linegap = 0; return; }
    int px = size_px < 1 ? 1 : size_px;
    cur_scale = stbtt_ScaleForPixelHeight(&fonts[cur].info, (float)px);
    stbtt_GetFontVMetrics(&fonts[cur].info, &cur_ascent, &cur_descent, &cur_linegap);
}

void ttf_reset(void) {
    for (int i = 0; i < MAX_FONTS; i++) fonts[i].used = 0;
    font_off = 0;
    cur = -1; size_px = 24;
    st_bold = st_italic = st_underline = 0;
    recompute_metrics();
}

int ttf_load(const char *name) {
    int slot = -1;
    for (int i = 0; i < MAX_FONTS; i++) if (!fonts[i].used) { slot = i; break; }
    if (slot < 0) return 0;                        // pool full

    int ch = stg_open(name, STG_M_READ);
    if (ch <= 0) return 0;
    long sz = stg_size(ch);
    stg_close(ch);
    if (sz <= 0) return 0;

    size_t start = (font_off + 15) & ~(size_t)15;
    if (start + (size_t)sz > FONT_ARENA_SIZE) return 0;   // arena full
    unsigned char *data = font_arena + start;
    if (stg_read(name, (char *)data, (int)sz) != (int)sz) return 0;

    int off = stbtt_GetFontOffsetForIndex(data, 0);
    if (off < 0) return 0;                          // not a font
    if (!stbtt_InitFont(&fonts[slot].info, data, off)) return 0;

    font_off = start + (size_t)sz;
    fonts[slot].used = 1;
    cur = slot;
    recompute_metrics();
    return slot + 1;                                // 1-based handle
}

int ttf_select(int handle) {
    int i = handle - 1;
    if (i < 0 || i >= MAX_FONTS || !fonts[i].used) return 0;
    cur = i;
    recompute_metrics();
    return 1;
}

void ttf_set_size(int pixels) { size_px = pixels < 1 ? 1 : pixels; recompute_metrics(); }
void ttf_set_style(int bold, int italic, int underline) {
    st_bold = bold ? 1 : 0; st_italic = italic ? 1 : 0; st_underline = underline ? 1 : 0;
}
int  ttf_ready(void) { return cur >= 0; }
int  ttf_style_flags(void) { return (st_bold ? 1 : 0) | (st_italic ? 2 : 0) | (st_underline ? 4 : 0); }
int  ttf_ascent(void)  { return cur < 0 ? 0 : (int)(cur_ascent * cur_scale + 0.5f); }
int  ttf_line_height(void) {
    if (cur < 0) return 0;
    return (int)((cur_ascent - cur_descent + cur_linegap) * cur_scale + 0.5f);
}

int ttf_text_width(const char *s, int len) {
    if (cur < 0) return 0;
    stbtt_fontinfo *f = &fonts[cur].info;
    double w = 0;
    for (int i = 0; i < len; i++) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(f, (unsigned char)s[i], &adv, &lsb);
        w += adv * cur_scale;
        if (i + 1 < len)
            w += stbtt_GetCodepointKernAdvance(f, (unsigned char)s[i], (unsigned char)s[i + 1]) * cur_scale;
    }
    if (st_bold) w += len;              // synthetic bold widens each glyph by ~1px
    return (int)(w + 0.5);
}

int ttf_glyph(int cp, const unsigned char **bitmap,
              int *w, int *h, int *xoff, int *yoff, int *advance) {
    *bitmap = 0; *w = *h = *xoff = *yoff = 0; *advance = 0;
    if (cur < 0) return 0;
    stbtt_fontinfo *f = &fonts[cur].info;

    scratch_reset();
    int gw = 0, gh = 0, gx = 0, gy = 0;
    unsigned char *bmp = stbtt_GetCodepointBitmap(f, cur_scale, cur_scale, cp, &gw, &gh, &gx, &gy);

    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(f, cp, &adv, &lsb);
    int adv_px = (int)(adv * cur_scale + 0.5f);

    if (st_bold && bmp && gw > 0 && gh > 0) {
        // Synthetic bold: dilate one pixel to the right (out = max(in, in[-1])).
        int nw = gw + 1;
        unsigned char *db = scratch_alloc((size_t)nw * gh);
        if (db) {
            for (int row = 0; row < gh; row++) {
                const unsigned char *src = bmp + row * gw;
                unsigned char *dst = db + row * nw;
                for (int x = 0; x < nw; x++) {
                    int a = (x < gw) ? src[x] : 0;
                    int b = (x > 0)  ? src[x - 1] : 0;
                    dst[x] = (unsigned char)(a > b ? a : b);
                }
            }
            bmp = db; gw = nw; adv_px += 1;
        }
    }

    *bitmap = bmp; *w = gw; *h = gh; *xoff = gx; *yoff = gy; *advance = adv_px;
    return 1;
}
