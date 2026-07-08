// Image loading via stb_image (bare-metal target backend).
//
// stb_image assumes a hosted libc: a heap (malloc/realloc/free) and a few string
// helpers. Freestanding, we supply those here — a bump "decode arena" for stb's
// scratch (reset after every load) and small str*/abs implementations — plus the
// <stdlib.h>/<string.h> shims in third_party/stb_shim/ (on this file's include
// path only). Finished sprites are copied into a separate persistent pool so the
// scratch can be reclaimed immediately; the pool is emptied by img_sprite_reset().
//
// Output format matches con_sprite_put (see image.h): width/height header then
// RGBA-ish pixels, bytes R,G,B,0xFF. All buffer writes are byte-wise for
// -mstrict-align safety.
#include <stddef.h>
#include <stdint.h>
#include "image.h"
#include "storage.h"

// --- decode arena: scratch for stb, reset after each load -------------------
// Sized for a full-screen image: compressed file + stb's raw scanline buffer +
// the decoded RGBA output all live here at once.
#define DEC_ARENA_SIZE  (24 * 1024 * 1024)
static unsigned char dec_arena[DEC_ARENA_SIZE] __attribute__((aligned(16)));
static size_t        dec_off;

static void dec_reset(void) { dec_off = 0; }

// Bump allocator with a 16-byte header holding the block size, so realloc can
// copy the old contents. free() is a no-op; dec_reset() reclaims everything.
static void *dec_alloc(size_t sz) {
    size_t start = (dec_off + 15) & ~(size_t)15;   // 16-align the header
    size_t user  = start + 16;                      // returned ptr is 16-aligned
    if (user + sz > DEC_ARENA_SIZE) return 0;
    *(size_t *)(dec_arena + start) = sz;
    dec_off = user + sz;
    return dec_arena + user;
}

void *img_malloc(size_t sz) { return dec_alloc(sz); }
void  img_free(void *p) { (void)p; }
void *img_realloc(void *p, size_t sz) {
    if (!p) return dec_alloc(sz);
    size_t old = *(size_t *)((unsigned char *)p - 16);
    unsigned char *n = dec_alloc(sz);
    if (!n) return 0;
    size_t c = old < sz ? old : sz;
    for (size_t i = 0; i < c; i++) n[i] = ((unsigned char *)p)[i];
    return n;
}

// --- tiny libc bits stb references (mem* come from kernel/util.c) ------------
int abs(int x) { return x < 0 ? -x : x; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) break;
    }
    return 0;
}

// --- stb_image (configured for freestanding, PNG/JPEG/BMP only) --------------
// STBI_NO_THREAD_LOCALS is ESSENTIAL on bare metal: without it stb makes its
// "flip vertically on load" flags __thread (thread-local) variables. There is no
// TLS runtime here, so __thread access goes through the uninitialised TPIDR_EL0
// register and reads garbage - on real hardware that garbage is non-zero, so
// every decoded image comes out upside-down (QEMU zeroes RAM/registers, hiding
// the bug). Forcing plain-global flags puts them in .bss, which boot zeroes.
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_ASSERT(x)      ((void)0)
#define STBI_MALLOC(sz)     img_malloc(sz)
#define STBI_REALLOC(p,sz)  img_realloc(p,sz)
#define STBI_FREE(p)        img_free(p)
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// --- stb_image_write (PNG/BMP encoding), same freestanding treatment ---------
#define STBI_WRITE_NO_STDIO
#define STBIW_ASSERT(x)     ((void)0)
#define STBIW_MALLOC(sz)    img_malloc(sz)
#define STBIW_REALLOC(p,sz) img_realloc(p,sz)
#define STBIW_FREE(p)       img_free(p)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// --- sprite pool: finished sprites, kept until img_sprite_reset() ------------
#define SPR_POOL_SIZE  (16 * 1024 * 1024)
static unsigned char spr_pool[SPR_POOL_SIZE] __attribute__((aligned(16)));
static size_t        spr_off;

void img_sprite_reset(void) { spr_off = 0; }

// Copy a decoded RGBA image into the sprite pool in GPUT format. Returns the
// sprite address, or 0 if the pool is full.
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
        // Keep the source alpha (byte 3) so GPUT can do transparency; images
        // without an alpha channel decode to 0xFF here (fully opaque).
        q[0] = rgba[0]; q[1] = rgba[1]; q[2] = rgba[2]; q[3] = rgba[3];
        q += 4; rgba += 4;
    }
    spr_off = start + need;
    return (long)(uintptr_t)o;
}

long img_load_sprite(const char *name) {
    dec_reset();

    // Read the whole compressed file into the decode arena.
    int ch = stg_open(name, STG_M_READ);
    if (ch <= 0) return 0;
    long sz = stg_size(ch);
    stg_close(ch);
    if (sz <= 0 || (size_t)sz > DEC_ARENA_SIZE / 2) return 0;
    unsigned char *file = dec_alloc((size_t)sz);
    if (!file) return 0;
    if (stg_read(name, (char *)file, (int)sz) != (int)sz) { dec_reset(); return 0; }

    // Decode to RGBA, then pack into the sprite pool.
    int w = 0, h = 0, comp = 0;
    unsigned char *px = stbi_load_from_memory(file, (int)sz, &w, &h, &comp, 4);
    long addr = 0;
    if (px && w > 0 && h > 0) addr = store_sprite(w, h, px);

    dec_reset();          // reclaim file + stb scratch + decoded pixels
    return addr;
}

// --- saving sprites ---------------------------------------------------------
// stb_image_write hands us the encoded file in chunks; we accumulate them into a
// decode-arena buffer, then write the whole file to storage in one go.
typedef struct { unsigned char *buf; int cap; int len; int ovf; } img_wctx;
static void img_write_cb(void *context, void *data, int size) {
    img_wctx *w = (img_wctx *)context;
    if (w->ovf || w->len + size > w->cap) { w->ovf = 1; return; }
    unsigned char *s = (unsigned char *)data;
    for (int i = 0; i < size; i++) w->buf[w->len + i] = s[i];
    w->len += size;
}

// Case-insensitive test for a filename ending in ".bmp".
static int name_is_bmp(const char *n) {
    int e = 0; while (n[e]) e++;
    if (e < 4) return 0;
    const char *x = n + e - 4;
    return x[0] == '.' &&
           (x[1] | 32) == 'b' && (x[2] | 32) == 'm' && (x[3] | 32) == 'p';
}

int img_save_sprite(const char *name, long addr) {
    if (!addr) return IMG_EARG;
    const unsigned char *p = (const unsigned char *)(uintptr_t)addr;
    int w = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    int h = p[4] | (p[5] << 8) | (p[6] << 16) | (p[7] << 24);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return IMG_EARG;
    const unsigned char *pixels = p + 8;   // RGBA, stride w*4

    dec_reset();
    // Output buffer: raw size plus generous headroom (PNG is normally smaller,
    // but an incompressible image can round up to ~raw + per-row overhead).
    size_t need = (size_t)w * (size_t)h * 4 + (size_t)w * (size_t)h + 4096;
    if (need > DEC_ARENA_SIZE / 2) { dec_reset(); return IMG_ETOOBIG; }
    img_wctx ctx;
    ctx.buf = dec_alloc(need);
    if (!ctx.buf) { dec_reset(); return IMG_ETOOBIG; }
    ctx.cap = (int)need; ctx.len = 0; ctx.ovf = 0;

    int ok = name_is_bmp(name)
             ? stbi_write_bmp_to_func(img_write_cb, &ctx, w, h, 4, pixels)
             : stbi_write_png_to_func(img_write_cb, &ctx, w, h, 4, pixels, w * 4);
    if (!ok || ctx.ovf) { dec_reset(); return IMG_EENCODE; }

    int wr = stg_write(name, (const char *)ctx.buf, ctx.len);
    dec_reset();
    return wr < 0 ? IMG_EIO : 0;
}
