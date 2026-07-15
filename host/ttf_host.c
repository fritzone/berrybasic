// Host (Linux) TrueType backend. Uses stb_truetype with the real libc, so the
// font-parsing and metric paths are exercised natively in the unit tests. Same
// interface as the target backend (drivers/ttf.c). The host has no framebuffer,
// so con_gtext is a no-op there, but LOADFONT / FONTSIZE / TEXTWIDTH / FONTHEIGHT
// all work and are testable.
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "ttf.h"
#include "storage.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define MAX_FONTS 8
static struct {
    int             used;
    unsigned char  *data;           // malloc'd TTF bytes (stbtt_fontinfo points in)
    stbtt_fontinfo  info;
} fonts[MAX_FONTS];

static int   cur = -1;
static int   size_px = 24;
static int   st_bold, st_italic, st_underline;
static float cur_scale;
static int   cur_ascent, cur_descent, cur_linegap;

static void recompute_metrics(void) {
    if (cur < 0) { cur_scale = 0; cur_ascent = cur_descent = cur_linegap = 0; return; }
    int px = size_px < 1 ? 1 : size_px;
    cur_scale = stbtt_ScaleForPixelHeight(&fonts[cur].info, (float)px);
    stbtt_GetFontVMetrics(&fonts[cur].info, &cur_ascent, &cur_descent, &cur_linegap);
}

void ttf_reset(void) {
    for (int i = 0; i < MAX_FONTS; i++)
        if (fonts[i].used) { free(fonts[i].data); fonts[i].data = 0; fonts[i].used = 0; }
    cur = -1; size_px = 24;
    st_bold = st_italic = st_underline = 0;
    recompute_metrics();
}

int ttf_load(const char *name) {
    int slot = -1;
    for (int i = 0; i < MAX_FONTS; i++) if (!fonts[i].used) { slot = i; break; }
    if (slot < 0) return 0;

    int ch = stg_open(name, STG_M_READ);
    if (ch <= 0) return 0;
    long sz = stg_size(ch);
    stg_close(ch);
    if (sz <= 0) return 0;

    unsigned char *data = (unsigned char *)malloc((size_t)sz);
    if (!data) return 0;
    if (stg_read(name, (char *)data, (int)sz) != (int)sz) { free(data); return 0; }

    int off = stbtt_GetFontOffsetForIndex(data, 0);
    if (off < 0 || !stbtt_InitFont(&fonts[slot].info, data, off)) { free(data); return 0; }

    fonts[slot].data = data;
    fonts[slot].used = 1;
    cur = slot;
    recompute_metrics();
    return slot + 1;
}

int ttf_select(int handle) {
    int i = handle - 1;
    if (i < 0 || i >= MAX_FONTS || !fonts[i].used) return 0;
    cur = i; recompute_metrics(); return 1;
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
    if (st_bold) w += len;
    return (int)(w + 0.5);
}

// Rasterize into a fixed static buffer (host has no framebuffer, so this exists
// only for completeness/testability; con_gtext is a no-op on the host).
int ttf_glyph(int cp, const unsigned char **bitmap,
              int *w, int *h, int *xoff, int *yoff, int *advance) {
    static unsigned char buf[512 * 512];
    *bitmap = 0; *w = *h = *xoff = *yoff = *advance = 0;
    if (cur < 0) return 0;
    stbtt_fontinfo *f = &fonts[cur].info;

    int gw = 0, gh = 0, gx = 0, gy = 0;
    unsigned char *bmp = stbtt_GetCodepointBitmap(f, cur_scale, cur_scale, cp, &gw, &gh, &gx, &gy);
    if (bmp && gw > 0 && gh > 0 && gw <= 512 && gh <= 512)
        memcpy(buf, bmp, (size_t)gw * gh);
    else { gw = gh = 0; }
    if (bmp) stbtt_FreeBitmap(bmp, 0);

    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(f, cp, &adv, &lsb);
    *bitmap = buf; *w = gw; *h = gh; *xoff = gx; *yoff = gy;
    *advance = (int)(adv * cur_scale + 0.5f) + (st_bold ? 1 : 0);
    return 1;
}
