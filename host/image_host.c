// Host (Linux) image-loading backend. Uses stb_image with the real libc, so the
// decode path can be exercised natively. Same interface and output format as the
// target backend (drivers/image.c); the host has no framebuffer, but a loaded
// sprite can still be inspected (SPRW/SPRH) and round-tripped in tests.
#include <stdlib.h>
#include <stdint.h>
#include "image.h"
#include "storage.h"

#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define SPR_POOL_SIZE  (16 * 1024 * 1024)
static unsigned char spr_pool[SPR_POOL_SIZE];
static size_t        spr_off;

void img_sprite_reset(void) { spr_off = 0; }

static long store_sprite(int w, int h, const unsigned char *rgba) {
    size_t need  = (size_t)w * (size_t)h * 4 + 8;
    size_t start = (spr_off + 15) & ~(size_t)15;
    if (start + need > SPR_POOL_SIZE) return 0;
    unsigned char *o = spr_pool + start;
    o[0] = w; o[1] = w >> 8; o[2] = w >> 16; o[3] = w >> 24;
    o[4] = h; o[5] = h >> 8; o[6] = h >> 16; o[7] = h >> 24;
    unsigned char *q = o + 8;
    size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        q[0] = rgba[0]; q[1] = rgba[1]; q[2] = rgba[2]; q[3] = rgba[3];  // keep alpha
        q += 4; rgba += 4;
    }
    spr_off = start + need;
    return (long)(uintptr_t)o;
}

long img_load_sprite(const char *name) {
    int ch = stg_open(name, STG_M_READ);
    if (ch <= 0) return 0;
    long sz = stg_size(ch);
    stg_close(ch);
    if (sz <= 0) return 0;
    unsigned char *file = malloc((size_t)sz);
    if (!file) return 0;
    if (stg_read(name, (char *)file, (int)sz) != (int)sz) { free(file); return 0; }

    int w = 0, h = 0, comp = 0;
    unsigned char *px = stbi_load_from_memory(file, (int)sz, &w, &h, &comp, 4);
    free(file);
    long addr = 0;
    if (px && w > 0 && h > 0) addr = store_sprite(w, h, px);
    stbi_image_free(px);
    return addr;
}

typedef struct { unsigned char *buf; int cap; int len; int ovf; } img_wctx;
static void img_write_cb(void *context, void *data, int size) {
    img_wctx *w = (img_wctx *)context;
    if (w->ovf || w->len + size > w->cap) { w->ovf = 1; return; }
    unsigned char *s = (unsigned char *)data;
    for (int i = 0; i < size; i++) w->buf[w->len + i] = s[i];
    w->len += size;
}
static int name_is_bmp(const char *n) {
    size_t e = 0; while (n[e]) e++;
    if (e < 4) return 0;
    const char *x = n + e - 4;
    return x[0] == '.' && (x[1] | 32) == 'b' && (x[2] | 32) == 'm' && (x[3] | 32) == 'p';
}

int img_save_sprite(const char *name, long addr) {
    if (!addr) return IMG_EARG;
    const unsigned char *p = (const unsigned char *)(uintptr_t)addr;
    int w = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    int h = p[4] | (p[5] << 8) | (p[6] << 16) | (p[7] << 24);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return IMG_EARG;
    const unsigned char *pixels = p + 8;

    size_t need = (size_t)w * (size_t)h * 4 + (size_t)w * (size_t)h + 4096;
    unsigned char *buf = malloc(need);
    if (!buf) return IMG_ETOOBIG;
    img_wctx ctx = { buf, (int)need, 0, 0 };
    int ok = name_is_bmp(name)
             ? stbi_write_bmp_to_func(img_write_cb, &ctx, w, h, 4, pixels)
             : stbi_write_png_to_func(img_write_cb, &ctx, w, h, 4, pixels, w * 4);
    int rc = 0;
    if (!ok || ctx.ovf) rc = IMG_EENCODE;
    else if (stg_write(name, (const char *)buf, ctx.len) < 0) rc = IMG_EIO;
    free(buf);
    return rc;
}
