#include "interp_pod.h"
#include "interp_util.h"
#include "interp_data.h"
#include "interp_lexer.h"
#include "interp_parse.h"
#include "interp_seed.h"
#include "interp_eval.h"
#include "interp_stmt.h"
#include "interp_files.h"
#include "interp_debug.h"
// ===========================================================================
// BerryBasiC — POD executables: the loader and the RUN/PODLOAD/PODFREE/PODINFO/
// PODCAPS verbs.
//
// This file is a fragment of the interpreter, #included by basic.c after the
// seed and storage sections (it reuses the svc_* service callbacks, the shared
// dynamic-keyword table seed_kw_tab, and stg_read). It is NOT a standalone
// translation unit.
//
// A POD is a self-contained native program (see seed/pod.h and doc "The POD
// Executable Format"): one flat, position-independent, checksummed image that
// declares up front which machine capabilities it needs. The loader verifies
// every checksum, copies the image into an executable slot, applies any
// relocations, and hands the POD a services table containing ONLY the groups it
// declared — every other slot is a refusal stub, so a POD cannot reach for a
// capability it did not ask for.
// ===========================================================================

unsigned char pod_pool[POD_MAX][POD_SLOT_SIZE] __attribute__((aligned(4096)));
unsigned char pod_filebuf[POD_SLOT_SIZE];   // whole-file staging for one load

pod_slot_t pod_slots[POD_MAX];

// --- CRC-32C (Castagnoli), matching the writer and the Cortex-A72 crc32cx ---
uint32_t pod_crc32c(const unsigned char *p, int n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0x82F63B78u & (uint32_t)-(int32_t)(crc & 1));
    }
    return ~crc;
}

// Little-endian readers (the file is LE; AArch64 is -mstrict-align so read bytewise).
uint16_t pod_rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t pod_rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t pod_rd64(const unsigned char *p) {
    return (uint64_t)pod_rd32(p) | ((uint64_t)pod_rd32(p + 4) << 32);
}
void pod_wr32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
void pod_wr64(unsigned char *p, uint64_t v) {
    pod_wr32(p, (uint32_t)v); pod_wr32(p + 4, (uint32_t)(v >> 32));
}

// ---------------------------------------------------------------------------
// Refusal stubs: what an ungranted capability slot points at. A well-formed POD
// never calls one (the compiler only lets it reference declared capabilities),
// but if it does the call is a harmless no-op / failure, never a real service.
// ---------------------------------------------------------------------------
void     pod_dn_puts(const char *a, int b)              { (void)a; (void)b; }
void     pod_dn_putc(int a)                             { (void)a; }
int      pod_dn_geti(void)                              { return 0; }
int      pod_dn_int_i(int a)                            { (void)a; return 0; }
int      pod_dn_getnum(const char *a, double *b)        { (void)a; if (b) *b = 0; return 0; }
void     pod_dn_setnum(const char *a, double b)         { (void)a; (void)b; }
int      pod_dn_getstr(const char *a, char *b, int c)   { (void)a; (void)b; (void)c; return 0; }
void     pod_dn_setstr(const char *a, const char *b, int c) { (void)a; (void)b; (void)c; }
int      pod_dn_fopen(const char *a, int b)             { (void)a; (void)b; return 0; }
int      pod_dn_fread(int a, void *b, int c)            { (void)a; (void)b; (void)c; return -1; }
int      pod_dn_fwrite(int a, const void *b, int c)     { (void)a; (void)b; (void)c; return -1; }
long     pod_dn_fseek(int a, long b, int c)             { (void)a; (void)b; (void)c; return -1; }
void     pod_dn_clear(uint32_t a)                       { (void)a; }
void     pod_dn_pixel(int a, int b, uint32_t c)         { (void)a; (void)b; (void)c; }
void     pod_dn_rect(int a, int b, int c, int d, uint32_t e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
int      pod_dn_gmode(int a, int b, int c)              { (void)a; (void)b; (void)c; return -1; }
void     pod_dn_gwrite(int a, int b)                    { (void)a; (void)b; }
uint32_t pod_dn_timecs(void)                            { return 0; }
void    *pod_dn_alloc(unsigned a)                       { (void)a; return 0; }
void     pod_dn_free(void *a)                           { (void)a; }
void    *pod_dn_realloc(void *a, unsigned b)            { (void)a; (void)b; return 0; }
void    *pod_dn_alloca(unsigned a, unsigned b)          { (void)a; (void)b; return 0; }
void     pod_dn_icache(const void *a, unsigned long b)  { (void)a; (void)b; }
void     pod_dn_blit8(const unsigned char *i, const unsigned int *p, int w, int h, int dx, int dy, int s) { (void)i; (void)p; (void)w; (void)h; (void)dx; (void)dy; (void)s; }
int      pod_dn_remove(const char *a)                   { (void)a; return -1; }
int      pod_dn_spawn(const char *a, int b, const char *const *c) { (void)a; (void)b; (void)c; return -1; }
int      pod_dn_mkdir(const char *a)                    { (void)a; return -1; }
int      pod_dn_diropen(const char *a)                  { (void)a; return 0; }   // 0 = failed
int      pod_dn_dirread(char *a, int b, int *c, long *d){ (void)a; (void)b; if (c) *c = 0; if (d) *d = 0; return 0; }
int      pod_dn_getcwd(char *a, int b)                  { if (a && b > 0) a[0] = 0; return -1; }
int      svc_pod_spawn(const char *path, int argc, const char *const *argv);
int      svc_pod_mkdir(const char *path) { return stg_mkdir(path); }
// --- refusal stubs for the ABI v4 surface (ungranted -> no-op / harmless) ---
double  *pod_dn_numarr(const char *a, int *b)           { (void)a; if (b) *b = 0; return 0; }
double  *pod_dn_recarr(const char *a, int *b, int *c)   { (void)a; if (b) *b = 0; if (c) *c = 0; return 0; }
int      pod_dn_recfield(const char *a, const char *b)  { (void)a; (void)b; return -1; }
int      pod_dn_recget(const char *a, int b, const char *c, char *d, int e) { (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }
void     pod_dn_recset(const char *a, int b, const char *c, const char *d, int e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
long     pod_dn_longi(int a)                            { (void)a; return -1; }
int      pod_dn_ii(int a, int b)                        { (void)a; (void)b; return -1; }
void     pod_dn_mouse(int *a, int *b, int *c)           { if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; }
int      pod_dn_keysdown(int *a, int b)                 { (void)a; (void)b; return -1; }
int      pod_dn_aopen(int r)                           { (void)r; return -1; }
int      pod_dn_geti0(void)                            { return 0; }
int      pod_dn_awrite(const short *s, int f)          { (void)s; return f; }
void     pod_dn_aclose(void)                          { }
void     pod_dn_scrsz(int *a, int *b)                   { if (a) *a = 0; if (b) *b = 0; }
void     pod_dn_glyph(int a, int b, int c, uint32_t d, uint32_t e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
uint32_t pod_dn_getpix(int a, int b)                    { (void)a; (void)b; return 0; }
void     pod_dn_circ(int a, int b, int c, uint32_t d)   { (void)a; (void)b; (void)c; (void)d; }
void     pod_dn_fpoly(const int *a, int b, uint32_t c)  { (void)a; (void)b; (void)c; }
void     pod_dn_clip4(int a, int b, int c, int d)       { (void)a; (void)b; (void)c; (void)d; }
void     pod_dn_void(void)                              { }
void     pod_dn_iii(int a, int b, int c)               { (void)a; (void)b; (void)c; }
void     pod_dn_pline(const int *a, int b, int c, uint32_t d) { (void)a; (void)b; (void)c; (void)d; }
int      pod_dn_fontload(const char *a)                 { (void)a; return 0; }
void     pod_dn_gtext(int a, int b, const char *c, int d, uint32_t e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
// --- refusal stubs for the debugger surface (ABI v25, ungranted CAP_DEBUG) ---
void pod_dn_dbgattach(void (*a)(const dbg_ctx *))       { (void)a; }
int  pod_dn_dbg_i_i(int a)                              { (void)a; return -1; }
int  pod_dn_dbg_s(const char *a)                        { (void)a; return -1; }
int  pod_dn_dbg_ss(const char *a, const char *b)        { (void)a; (void)b; return -1; }
void pod_dn_dbg_v_i(int a)                              { (void)a; }
int  pod_dn_dbg_list(int *a, int b)                     { (void)a; (void)b; return 0; }
int  pod_dn_dbg_lineif(int a, const char *b, int c)     { (void)a; (void)b; (void)c; return -1; }
void pod_dn_dbg_traceto(int a, int b)                   { (void)a; (void)b; }
int  pod_dn_dbg_where(dbg_ctx *a)                       { (void)a; return -1; }
int  pod_dn_dbg_eval(const char *a, double *b, char *c, int d) { (void)a; if (b) *b = 0; (void)c; (void)d; return -1; }
int  pod_dn_dbg_setnum(const char *a, double b)         { (void)a; (void)b; return -1; }
int  pod_dn_dbg_setstr(const char *a, const char *b, int c) { (void)a; (void)b; (void)c; return -1; }
int  pod_dn_dbg_varat(int a, char *b, int c, int *d, int *e) { (void)a; if (b && c > 0) b[0] = 0; if (d) *d = 0; if (e) *e = 0; return -1; }
int  pod_dn_dbg_frame(int a, char *b, int c, int *d)    { (void)a; if (b && c > 0) b[0] = 0; if (d) *d = 0; return -1; }
int  pod_dn_dbg_lineat(int a, int *b, char *c, int d)   { (void)a; if (b) *b = 0; if (c && d > 0) c[0] = 0; return -1; }

// Fill a services table: every slot a refusal stub, then the granted groups
// overwritten with the real service callbacks (defined in interp_seed.inc).
void pod_build_svc(BerryServices *s, uint64_t caps) {
    s->abi_version = BERRY_ABI_VERSION;
    s->puts = pod_dn_puts; s->putc = pod_dn_putc; s->getkey = pod_dn_geti; s->inkey = pod_dn_int_i;
    s->get_num = pod_dn_getnum; s->set_num = pod_dn_setnum;
    s->get_str = pod_dn_getstr; s->set_str = pod_dn_setstr;
    s->file_open = pod_dn_fopen; s->file_close = pod_dn_int_i;
    s->file_read = pod_dn_fread; s->file_write = pod_dn_fwrite; s->file_seek = pod_dn_fseek;
    s->gfx_width = pod_dn_geti; s->gfx_height = pod_dn_geti; s->gfx_clear = pod_dn_clear;
    s->gfx_putpixel = pod_dn_pixel; s->gfx_line = pod_dn_rect; s->gfx_fillrect = pod_dn_rect;
    s->gfx_blit8 = pod_dn_blit8;
    s->gpio_mode = pod_dn_gmode; s->gpio_write = pod_dn_gwrite; s->gpio_read = pod_dn_int_i;
    s->time_cs = pod_dn_timecs;
    s->alloc = pod_dn_alloc; s->free = pod_dn_free;
    s->realloc = pod_dn_realloc; s->alloc_aligned = pod_dn_alloca;
    s->icache_sync = pod_dn_icache;
    s->file_remove = pod_dn_remove;
    s->spawn = pod_dn_spawn; s->mkdir = pod_dn_mkdir;
    s->dir_open = pod_dn_diropen; s->dir_read = pod_dn_dirread;
    s->getcwd = pod_dn_getcwd; s->chdir = pod_dn_mkdir;   // chdir refusal: -1
    // v4 defaults (all refusals; overwritten below by the granted groups)
    s->num_array = pod_dn_numarr; s->rec_array = pod_dn_recarr; s->rec_field = pod_dn_recfield;
    s->rec_get_str = pod_dn_recget; s->rec_set_str = pod_dn_recset;
    s->file_size = pod_dn_longi; s->file_eof = pod_dn_int_i;
    s->gpio_avail = pod_dn_geti; s->gpio_pull = pod_dn_ii; s->gpio_set = pod_dn_clear;
    s->gpio_clr = pod_dn_clear; s->gpio_level = pod_dn_timecs; s->gpio_wait = pod_dn_gmode;
    s->mouse = pod_dn_mouse; s->keymods = pod_dn_geti; s->keys_down = pod_dn_keysdown;
    s->audio_open = pod_dn_aopen; s->audio_avail = pod_dn_geti0;
    s->audio_write = pod_dn_awrite; s->audio_close = pod_dn_aclose;
    s->gfx_avail = pod_dn_geti; s->gfx_getpixel = pod_dn_getpix;
    s->gfx_circle = pod_dn_circ; s->gfx_fillcircle = pod_dn_circ;
    s->gfx_ellipse = pod_dn_rect; s->gfx_fillellipse = pod_dn_rect;
    s->gfx_fillpoly = pod_dn_fpoly; s->gfx_flood = pod_dn_pixel;
    s->gfx_clip = pod_dn_clip4; s->gfx_noclip = pod_dn_void;
    s->gfx_line_style = pod_dn_iii; s->gfx_polyline = pod_dn_pline;
    s->gfx_mode = pod_dn_geti; s->gfx_backbuffer = pod_dn_int_i;
    s->gfx_flip = pod_dn_void; s->gfx_buffered = pod_dn_geti;
    s->font_load = pod_dn_fontload; s->font_select = pod_dn_int_i;
    s->font_size = pod_dn_putc; s->font_style = pod_dn_iii;
    s->gfx_text = pod_dn_gtext; s->text_width = pod_dn_fopen; s->text_height = pod_dn_geti;
    s->fmt_num = svc_fmt_num;                       // pure formatter: always available
    s->set_return_str = pod_dn_puts;
    s->vdu = pod_dn_putc;                           // (int)->void, harmless refusal
    s->screen_size = pod_dn_scrsz;
    s->con_font = pod_dn_scrsz; s->con_glyph = pod_dn_glyph;
    s->clip_set = pod_dn_puts; s->clip_get = pod_dn_getcwd; s->clip_len = pod_dn_geti;
    // v25 debugger defaults: all refusals until CAP_DEBUG grants the real ones.
    s->dbg_attach = pod_dn_dbgattach; s->dbg_detach = pod_dn_void;
    s->dbg_break_line = pod_dn_dbg_i_i; s->dbg_break_proc = pod_dn_dbg_s;
    s->dbg_break_kw = pod_dn_dbg_s; s->dbg_break_every = pod_dn_dbg_v_i;
    s->dbg_clear = pod_dn_dbg_v_i; s->dbg_list_breaks = pod_dn_dbg_list;
    s->dbg_cont = pod_dn_void; s->dbg_step = pod_dn_void;
    s->dbg_step_over = pod_dn_void; s->dbg_step_out = pod_dn_void; s->dbg_abort = pod_dn_void;
    s->dbg_watch = pod_dn_dbg_ss; s->dbg_break_error = pod_dn_dbg_v_i;
    s->dbg_break_line_if = pod_dn_dbg_lineif;
    s->dbg_trace_to = pod_dn_dbg_traceto; s->dbg_trace_off = pod_dn_void;
    s->dbg_where = pod_dn_dbg_where; s->dbg_eval = pod_dn_dbg_eval;
    s->dbg_set_num = pod_dn_dbg_setnum; s->dbg_set_str = pod_dn_dbg_setstr;
    s->dbg_var_count = pod_dn_geti; s->dbg_var_at = pod_dn_dbg_varat;
    s->dbg_stack_depth = pod_dn_geti; s->dbg_stack_frame = pod_dn_dbg_frame;
    s->dbg_line_count = pod_dn_geti; s->dbg_line_at = pod_dn_dbg_lineat;
    s->dbg_run = pod_dn_dbg_s;

    if (caps & POD_CAP_CONSOLE) {
        s->puts = svc_puts; s->putc = svc_putc; s->getkey = svc_getkey; s->inkey = svc_inkey;
        s->mouse = svc_mouse; s->keymods = svc_keymods; s->keys_down = svc_keys_down; s->vdu = svc_vdu;
        s->audio_open = svc_audio_open; s->audio_avail = svc_audio_avail;
        s->audio_write = svc_audio_write; s->audio_close = svc_audio_close;
        s->screen_size = svc_screen_size;
        s->clip_set = svc_clip_set; s->clip_get = svc_clip_get; s->clip_len = svc_clip_len;
    }
    if (caps & POD_CAP_VARS) {
        s->get_num = svc_get_num; s->set_num = svc_set_num;
        s->get_str = svc_get_str; s->set_str = svc_set_str;
        s->num_array = svc_num_array;
        s->rec_array = svc_rec_array; s->rec_field = svc_rec_field;
        s->rec_get_str = svc_rec_get_str; s->rec_set_str = svc_rec_set_str;
    }
    if (caps & POD_CAP_FILES) {
        s->file_open = svc_file_open; s->file_close = svc_file_close;
        s->file_read = svc_file_read; s->file_write = svc_file_write; s->file_seek = svc_file_seek;
        s->file_remove = svc_file_remove; s->file_size = svc_file_size; s->file_eof = svc_file_eof;
    }
    if (caps & POD_CAP_GRAPHICS) {
        s->gfx_width = svc_gfx_width; s->gfx_height = svc_gfx_height; s->gfx_clear = svc_gfx_clear;
        s->gfx_putpixel = svc_gfx_putpixel; s->gfx_line = svc_gfx_line; s->gfx_fillrect = svc_gfx_fillrect;
        s->gfx_blit8 = svc_gfx_blit8;
        s->gfx_avail = svc_gfx_avail; s->gfx_getpixel = svc_gfx_getpixel;
        s->gfx_circle = svc_gfx_circle; s->gfx_fillcircle = svc_gfx_fillcircle;
        s->gfx_ellipse = svc_gfx_ellipse; s->gfx_fillellipse = svc_gfx_fillellipse;
        s->gfx_fillpoly = svc_gfx_fillpoly; s->gfx_flood = svc_gfx_flood;
        s->gfx_clip = svc_gfx_clip; s->gfx_noclip = svc_gfx_noclip;
        s->gfx_line_style = svc_gfx_line_style; s->gfx_polyline = svc_gfx_polyline;
        s->gfx_mode = svc_gfx_mode; s->gfx_backbuffer = svc_gfx_backbuffer;
        s->gfx_flip = svc_gfx_flip; s->gfx_buffered = svc_gfx_buffered;
        s->font_load = svc_font_load; s->font_select = svc_font_select;
        s->font_size = svc_font_size; s->font_style = svc_font_style;
        s->gfx_text = svc_gfx_text; s->text_width = svc_text_width; s->text_height = svc_text_height;
        s->con_font = svc_con_font; s->con_glyph = svc_con_glyph;
    }
    if (caps & POD_CAP_GPIO) {
        s->gpio_mode = svc_gpio_mode; s->gpio_write = svc_gpio_write; s->gpio_read = svc_gpio_read;
        s->gpio_avail = svc_gpio_avail; s->gpio_pull = svc_gpio_pull;
        s->gpio_set = svc_gpio_set; s->gpio_clr = svc_gpio_clr;
        s->gpio_level = svc_gpio_level; s->gpio_wait = svc_gpio_wait;
    }
    if (caps & POD_CAP_TIME) s->time_cs = svc_time_cs;
    if (caps & POD_CAP_HEAP) { s->alloc = svc_alloc; s->free = svc_free; s->realloc = svc_realloc; s->alloc_aligned = svc_alloc_aligned; s->icache_sync = svc_icache_sync; }
    if (caps & POD_CAP_SPAWN) { s->spawn = svc_pod_spawn; s->run_basic = svc_run_basic; }
    if (caps & POD_CAP_DIRS) {
        s->mkdir = svc_pod_mkdir;
        s->dir_open = svc_dir_open; s->dir_read = svc_dir_read;
        s->getcwd = svc_getcwd; s->chdir = svc_chdir;
    }
    if (caps & POD_CAP_KEYWORD) s->set_return_str = svc_set_return_str;
    if (caps & POD_CAP_DEBUG) {
        s->dbg_attach = dbg_svc_attach; s->dbg_detach = dbg_svc_detach;
        s->dbg_break_line = dbg_svc_break_line; s->dbg_break_proc = dbg_svc_break_proc;
        s->dbg_break_kw = dbg_svc_break_kw; s->dbg_break_every = dbg_svc_break_every;
        s->dbg_clear = dbg_svc_clear; s->dbg_list_breaks = dbg_svc_list_breaks;
        s->dbg_cont = dbg_do_cont; s->dbg_step = dbg_do_step;
        s->dbg_step_over = dbg_do_step_over; s->dbg_step_out = dbg_do_step_out; s->dbg_abort = dbg_do_abort;
        s->dbg_watch = dbg_svc_watch; s->dbg_break_error = dbg_svc_break_error;
        s->dbg_break_line_if = dbg_svc_break_line_if;
        s->dbg_trace_to = dbg_svc_trace_to; s->dbg_trace_off = dbg_svc_trace_off;
        s->dbg_where = dbg_svc_where; s->dbg_eval = dbg_eval_expr;
        s->dbg_set_num = dbg_svc_set_num; s->dbg_set_str = dbg_svc_set_str;
        s->dbg_var_count = dbg_svc_var_count; s->dbg_var_at = dbg_svc_var_at;
        s->dbg_stack_depth = dbg_svc_stack_depth; s->dbg_stack_frame = dbg_svc_stack_frame;
        s->dbg_line_count = dbg_svc_line_count; s->dbg_line_at = dbg_svc_line_at;
        s->dbg_run = dbg_svc_run;
    }
}

// ---------------------------------------------------------------------------
// A parsed POD file. All pointers are into pod_filebuf, valid until the next
// load. pod_open() fills it (verifying every checksum) or raises and returns 0.
// ---------------------------------------------------------------------------

// Read a POD file into pod_filebuf and verify it end to end (magic, header CRC,
// declared file_size, every chunk CRC, payload CRC and the SEAL). Fills *im.
// Returns 1 on success; on failure raises a BASIC error and returns 0. With
// silent_missing set, a not-found file returns -1 WITHOUT raising, so a caller
// probing for an optional file (the /sys command dispatch) can fall through.
int pod_open(const char *path, pod_image_t *im, int silent_missing) {
    for (int i = 0; i < (int)sizeof *im; i++) ((char *)im)[i] = 0;

    int len = stg_read(path, (char *)pod_filebuf, POD_SLOT_SIZE);
    if (len < 0)  {
        if (silent_missing && len == STG_ENOTFOUND) return -1;
        stg_err(len); return 0;
    }
    if (len == 0 || len < 64) { err("Not a POD file"); return 0; }
    const unsigned char *d = pod_filebuf;

    if (d[0] != POD_MAGIC0 || d[1] != POD_MAGIC1 || d[2] != POD_MAGIC2 || d[3] != POD_MAGIC3
        || d[4] != 0x0D || d[5] != 0x0A || d[6] != 0x1A || d[7] != 0x0A) {
        err("Not a POD file"); return 0;
    }
    if (pod_crc32c(d, 60) != pod_rd32(d + 60)) { err("POD header is damaged"); return 0; }
    if (pod_rd16(d + 8) > POD_FORMAT_VERSION || pod_rd16(d + 10) > BERRY_ABI_VERSION) {
        err("POD needs a newer system"); return 0;
    }
    if ((int)pod_rd32(d + 16) != len) { err("POD is truncated"); return 0; }

    im->flags      = pod_rd16(d + 14);
    im->image_size = pod_rd32(d + 20);
    im->split_off  = pod_rd32(d + 24);
    im->init_size  = pod_rd32(d + 28);
    im->entry_off  = pod_rd32(d + 32);
    im->caps       = pod_rd64(d + 44);
    im->build_epoch= pod_rd32(d + 52);
    im->abi        = pod_rd16(d + 10);

    if ((im->split_off & 0xFFF) || im->init_size > im->image_size
        || im->image_size > POD_SLOT_SIZE) { err("POD image is malformed"); return 0; }
    if (!(im->flags & POD_KIND_EXTENSION) && im->entry_off >= im->split_off) {
        err("POD image is malformed"); return 0;
    }

    // Walk the chunks forward once. Verify each chunk's CRC; note where SEAL
    // starts so the payload/whole CRCs can be checked over the right ranges.
    int off = 64, seal_start = -1, have_seal = 0;
    int nchunks = 0, want = pod_rd16(d + 12);
    while (off + 12 <= len) {
        const unsigned char *c = d + off;
        int size = (int)pod_rd32(c + 4);
        uint32_t ccrc = pod_rd32(c + 8);
        const unsigned char *pl = c + 12;
        if (off + 12 + size > len) { err("POD is truncated"); return 0; }
        if (pod_crc32c(pl, size) != ccrc) { err("POD is damaged"); return 0; }
        if      (c[0]=='I'&&c[1]=='M'&&c[2]=='A'&&c[3]=='G') { im->imag = pl; im->imag_len = size; }
        else if (c[0]=='K'&&c[1]=='E'&&c[2]=='Y'&&c[3]=='W') { im->keyw = pl; im->keyw_len = size; }
        else if (c[0]=='R'&&c[1]=='L'&&c[2]=='O'&&c[3]=='C') { im->rloc = pl; im->rloc_len = size; }
        else if (c[0]=='M'&&c[1]=='A'&&c[2]=='R'&&c[3]=='K') { im->mark = pl; im->mark_len = size; }
        else if (c[0]=='N'&&c[1]=='E'&&c[2]=='E'&&c[3]=='D') { im->need = pl; im->need_len = size; }
        else if (c[0]=='S'&&c[1]=='E'&&c[2]=='A'&&c[3]=='L') { seal_start = off; have_seal = 1; }
        nchunks++;
        off += 12 + size;
        off = (off + 3) & ~3;                         // pad to 4
        if (have_seal) break;                         // SEAL is always last
    }
    if (!have_seal)         { err("POD is not sealed"); return 0; }
    if (nchunks != want)    { err("POD is damaged"); return 0; }
    if (!im->imag || im->imag_len != (int)im->init_size) { err("POD image is malformed"); return 0; }

    if (pod_crc32c(d + 64, seal_start - 64) != pod_rd32(d + 56)) { err("POD is damaged"); return 0; }
    // SEAL: method (byte 0) + whole_crc over [0, seal_start) at payload+4.
    const unsigned char *seal = d + seal_start + 12;
    if (pod_crc32c(d, seal_start) != pod_rd32(seal + 4)) { err("POD is damaged"); return 0; }
    return 1;
}

// Copy a verified image into slot `si`, apply relocations, make it executable,
// and build its services table. Returns the image base pointer.
unsigned char *pod_instantiate(int si, const pod_image_t *im) {
    unsigned char *base = pod_pool[si];
    for (uint32_t i = 0; i < im->image_size; i++) base[i] = 0;       // clears BSS
    for (int i = 0; i < im->imag_len; i++) base[i] = im->imag[i];

    // Apply RLOC: add the load base to each stored value (a POD is bound at 0).
    if (im->rloc && im->rloc_len >= 4) {
        int n = (int)pod_rd32(im->rloc);
        uint64_t addr = (uint64_t)(uintptr_t)base;
        for (int i = 0; i < n; i++) {
            const unsigned char *e = im->rloc + 4 + i * 8;
            uint32_t o = pod_rd32(e);
            int kind = e[4];
            if (o + 4 > im->image_size) continue;
            if (kind == POD_RELOC_ABS64) {
                if (o + 8 <= im->image_size) pod_wr64(base + o, pod_rd64(base + o) + addr);
            } else {                                    // ABS32 / REL32
                pod_wr32(base + o, pod_rd32(base + o) + (uint32_t)addr);
            }
        }
    }

    icache_sync(base, im->split_off ? im->split_off : im->init_size);   // RX region
    pod_slots[si].caps       = im->caps;
    pod_slots[si].image_size = im->image_size;
    pod_slots[si].split_off  = im->split_off;
    pod_slots[si].init_size  = im->init_size;
    pod_slots[si].entry_off  = im->entry_off;
    pod_slots[si].flags      = im->flags;
    pod_build_svc(&pod_slots[si].svc, im->caps);
    return base;
}

int pod_free_slot(void) {
    for (int i = 0; i < POD_MAX; i++) if (!pod_slots[i].used) return i;
    return -1;
}

// The MARK value for a key (e.g. "name"), copied NUL-terminated into out. MARK
// is a run of NUL-terminated "key=value" records. Returns 1 if found.
int pod_mark_get(const pod_image_t *im, const char *key, char *out, int outsz) {
    if (!im->mark) return 0;
    const unsigned char *p = im->mark, *end = p + im->mark_len;
    while (p < end) {
        const unsigned char *rec = p;
        while (p < end && *p) p++;                     // to the record's NUL
        const unsigned char *eq = rec;
        while (eq < p && *eq != '=') eq++;
        if (eq < p) {
            int kl = (int)(eq - rec), ok = 1;
            for (int i = 0; key[i] || i < kl; i++)
                if (key[i] != (i < kl ? (char)rec[i] : 0)) { ok = 0; break; }
            if (ok) {
                int j = 0; const unsigned char *v = eq + 1;
                while (v < p && j < outsz - 1) out[j++] = (char)*v++;
                out[j] = 0;
                return 1;
            }
        }
        p++;                                           // past the NUL
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Resolve a POD file name argument (a string expression) into a path, defaulting
// the extension to .POD. Returns 1 on success with the path in `path`.
// ---------------------------------------------------------------------------
// PODLOAD/PODINFO/RUN "NAME.POD": a quoted or bare path, defaulting to .POD. These
// are typed as commands, so a bare name (PODINFO HELLO) is the norm. The keyword
// is already consumed and the path is the current token, so rewind the cursor to
// its start and read it raw.
int pod_arg_path(char *path, int pathsz) {
    lx = tok_start;
    return read_path(path, pathsz, ".POD");
}

// Same .POD default, but from a string *expression* - for PODCAPS(...), which is
// a value function used inside expressions where a variable (PODCAPS(f$)) or a
// built path must still evaluate rather than be taken literally.
int pod_expr_path(char *path, int pathsz) {
    value_t v = eval_expr();
    if (g_err) return 0;
    if (!v.is_str) { err("Expected a file name"); return 0; }
    int p = 0, has_dot = 0;
    for (int i = 0; i < v.len && p < pathsz - 5; i++) { path[p++] = v.str[i]; if (v.str[i] == '.') has_dot = 1; }
    path[p] = 0;
    if (!has_dot && p + 4 < pathsz) { path[p++]='.'; path[p++]='P'; path[p++]='O'; path[p++]='D'; path[p]=0; }
    return 1;
}

// ===========================================================================
// Running a program POD: RUN "NAME.POD" [, args$], and the /sys command shell.
// ===========================================================================
double g_pod_status;         // exit status of the last program POD

// argv storage for one program run (a POD sees these for the duration of pod_main).
char        pod_argbuf[LINE_LEN + 16];
const char *pod_argv[40];

// Split a raw command tail into argv[1..], with argv[0] set to `name`. Tokens are
// whitespace-separated; a "double-quoted" run is one token with the quotes
// stripped (so file names with spaces survive). Returns argc.
int pod_split_args(const char *name, const char *raw) {
    int argc = 0, w = 0;
    // argv[0] = the command / program name
    pod_argv[argc++] = pod_argbuf + w;
    for (int i = 0; name && name[i] && w < (int)sizeof pod_argbuf - 1; i++) pod_argbuf[w++] = name[i];
    pod_argbuf[w++] = 0;

    const char *p = raw ? raw : "";
    while (*p && argc < POD_MAX_ARGV) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        pod_argv[argc++] = pod_argbuf + w;
        if (*p == '"') {                                 // quoted token
            p++;
            while (*p && *p != '"' && w < (int)sizeof pod_argbuf - 1) pod_argbuf[w++] = *p++;
            if (*p == '"') p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && w < (int)sizeof pod_argbuf - 1) pod_argbuf[w++] = *p++;
        }
        pod_argbuf[w++] = 0;
    }
    return argc;
}

// Instantiate an already-verified program POD in a transient slot, run its
// pod_main(svc, argc, argv), free the slot, and record the exit status.
void pod_exec_program(pod_image_t *im, int argc, const char *const *argv) {
    if (im->flags & POD_KIND_EXTENSION) { err("That POD is an extension; use PODLOAD"); return; }
    int si = pod_free_slot();
    if (si < 0) { err("Too many PODs loaded"); return; }
    pod_slots[si].used = 1; pod_slots[si].resident = 0;
    unsigned char *base = pod_instantiate(si, im);
    int status = 0;
    int rc = pod_run_main(base + im->entry_off, &pod_slots[si].svc, argc, argv, &status);
    pod_slots[si].used = 0;                              // program PODs are transient
    if (rc != 0) { err("PODs run on the Pi, not the host build"); return; }
    g_pod_status = (double)status;
}

// The CAP_SPAWN service: run a program POD nested inside another (a build tool
// spawning the compiler). Returns the spawned POD's exit status, or a negative
// code on failure. It saves/restores g_err so a load failure is reported to the
// caller as a return value rather than aborting the BASIC program; the spawned
// POD runs in its own slot with its own capability-gated table.
int pod_spawn(const char *path, int argc, const char *const *argv) {
    int saved_err = g_err;
    g_err = 0;
    pod_image_t im;
    int r = pod_open(path, &im, 1);
    if (r <= 0)                        { g_err = saved_err; return -1; }  // missing/damaged
    if (im.flags & POD_KIND_EXTENSION) { g_err = saved_err; return -2; }  // not a program
    int si = pod_free_slot();
    if (si < 0)                        { g_err = saved_err; return -3; }  // no free slot
    pod_slots[si].used = 1; pod_slots[si].resident = 0;
    unsigned char *base = pod_instantiate(si, &im);
    int status = 0;
    int rc = pod_run_main(base + im.entry_off, &pod_slots[si].svc, argc, argv, &status);
    pod_slots[si].used = 0;
    g_err = saved_err;
    return rc != 0 ? -4 : status;
}
int svc_pod_spawn(const char *path, int argc, const char *const *argv) {
    return pod_spawn(path, argc, argv);
}

// basename of a path (after the last '/'), minus any extension, into out.
void pod_basename(const char *path, char *out, int outsz) {
    const char *b = path;
    for (const char *q = path; *q; q++) if (*q == '/') b = q + 1;
    int n = 0;
    for (; b[n] && b[n] != '.' && n < outsz - 1; n++) out[n] = b[n];
    out[n] = 0;
}

// RUN "NAME.POD" [, args$] — load and execute a program POD.
void pod_run_program(void) {
    char path[96];
    if (!pod_arg_path(path, sizeof path)) return;

    char args[LINE_LEN]; args[0] = 0;
    if (tok == T_COMMA) {
        lex_next();
        value_t a = eval_expr();
        if (g_err) return;
        if (!a.is_str) { err("POD arguments must be a string"); return; }
        int n = a.len < (int)sizeof(args) - 1 ? a.len : (int)sizeof(args) - 1;
        for (int i = 0; i < n; i++) args[i] = a.str[i];
        args[n] = 0;
    }

    pod_image_t im;
    if (!pod_open(path, &im, 0)) return;
    char name[16]; pod_basename(path, name, sizeof name);
    int argc = pod_split_args(name, args);
    pod_exec_program(&im, argc, pod_argv);
}

// ---------------------------------------------------------------------------
// The /sys command shell: a bare word (or RUN word) whose name matches a POD in
// /sys is run as a command, the rest of the line passed as its arguments. This
// is what makes the machine command-driven: `tcc -pod hello.c -o HELLO.POD`
// finds /sys/TCC.POD and runs it with argv = { "tcc", "-pod", ... }.
//
// `name` is the command word (already the identifier the lexer read); `rawtail`
// is the raw text after it, up to end of line. Returns 1 if it was a /sys
// command (ran, or raised a real error); 0 if there is no such command (so the
// caller falls back to treating the word as a variable).
int sys_try_command(const char *name, const char *rawtail) {
    char path[64];
    int p = 0; const char *pre = "/sys/";
    while (*pre) path[p++] = *pre++;
    for (int i = 0; name[i] && p < (int)sizeof path - 6; i++) path[p++] = name[i];
    const char *ext = ".POD";
    for (int i = 0; ext[i]; i++) path[p++] = ext[i];
    path[p] = 0;

    pod_image_t im;
    int r = pod_open(path, &im, 1);                     // silent if not present
    if (r < 0) return 0;                                // no such /sys command
    if (r == 0) return 1;                               // present but invalid: error raised
    int argc = pod_split_args(name, rawtail);
    pod_exec_program(&im, argc, pod_argv);
    return 1;
}

// ===========================================================================
// PODLOAD "NAME.POD" — load an extension POD and register its keywords.
// ===========================================================================
void stmt_podload(void) {
    lex_next();                                         // consume PODLOAD
    char path[96];
    if (!pod_arg_path(path, sizeof path)) return;

    pod_image_t im;
    if (!pod_open(path, &im, 0)) return;
    if (!(im.flags & POD_KIND_EXTENSION)) { err("That POD is a program; use RUN"); return; }
    if (!im.keyw || im.keyw_len < 4)      { err("This POD registers no keywords"); return; }

    char nm[16]; if (!pod_mark_get(&im, "name", nm, sizeof nm)) { nm[0] = 0; }
    // already loaded (same name resident)? then it is a no-op.
    for (int i = 0; i < POD_MAX; i++)
        if (pod_slots[i].used && pod_slots[i].resident && s_eq(pod_slots[i].name, nm)) return;

    int nk = (int)pod_rd32(im.keyw);
    if (seed_kw_n + nk > SEED_KW_MAX) { err("Too many keywords loaded"); return; }
    // Reject if any name already exists (a seed or another POD owns it).
    for (int i = 0; i < nk; i++) {
        const unsigned char *r = im.keyw + 4 + i * 24;
        char name[16]; int nlen = 0;
        for (int j = 0; j < 15 && r[j]; j++) name[nlen++] = up(r[j]);
        name[nlen] = 0;
        if (nlen == 0 || seed_kw_lookup(name) >= 0) { err("A keyword name is already in use"); return; }
    }

    int si = pod_free_slot();
    if (si < 0) { err("Too many PODs loaded"); return; }
    pod_slots[si].used = 1; pod_slots[si].resident = 1;
    s_copy(pod_slots[si].name, nm, 16);
    unsigned char *base = pod_instantiate(si, &im);

    for (int i = 0; i < nk; i++) {
        const unsigned char *r = im.keyw + 4 + i * 24;
        char name[16]; int nlen = 0;
        for (int j = 0; j < 15 && r[j]; j++) name[nlen++] = up(r[j]);
        name[nlen] = 0;
        int kind = r[12]; if (kind < 0 || kind > POD_KW_STRFN) kind = POD_KW_STATEMENT;
        int mn = r[13], mx = r[14];
        if (mn > SEED_MAX_ARGS) mn = SEED_MAX_ARGS;
        if (mx > SEED_MAX_ARGS) mx = SEED_MAX_ARGS;
        if (mx < mn) mx = mn;
        uint32_t hoff = pod_rd32(r + 16);

        seed_kw_t *k = &seed_kw_tab[seed_kw_n];
        s_copy(k->name, name, 16);
        k->kind = kind; k->minargs = mn; k->maxargs = mx; k->slot = si;
        k->is_pod = 1;
        k->entry  = 0;                          // POD keyword: runs via pod_entry
        k->pod_entry = base + hoff;
        k->pod_svc   = &pod_slots[si].svc;
        seed_kw_n++;
    }
}

// Remove every keyword owned by slot `si` from the shared table (compacting it).
void pod_unregister_keywords(int si) {
    int w = 0;
    for (int r = 0; r < seed_kw_n; r++) {
        if (seed_kw_tab[r].is_pod && seed_kw_tab[r].slot == si) continue;   // drop it
        if (w != r) seed_kw_tab[w] = seed_kw_tab[r];
        w++;
    }
    seed_kw_n = w;
}

// PODFREE "NAME" — unload a resident extension POD by its (MARK) name.
void stmt_podfree(void) {
    lex_next();                                         // consume PODFREE
    value_t v = eval_expr();
    if (g_err) return;
    if (!v.is_str) { err("Expected a POD name"); return; }
    char nm[16]; int n = v.len < 15 ? v.len : 15;
    for (int i = 0; i < n; i++) nm[i] = v.str[i];
    nm[n] = 0;
    for (int i = 0; i < POD_MAX; i++)
        if (pod_slots[i].used && pod_slots[i].resident && s_eq(pod_slots[i].name, nm)) {
            pod_unregister_keywords(i);
            pod_slots[i].used = 0; pod_slots[i].resident = 0;
            return;
        }
    err("No such POD is loaded");
}

// Drop every loaded POD and its keywords (called on NEW).
void pod_reset(void) {
    for (int i = 0; i < POD_MAX; i++) {
        if (pod_slots[i].used && pod_slots[i].resident) pod_unregister_keywords(i);
        pod_slots[i].used = 0; pod_slots[i].resident = 0;
    }
}

// ===========================================================================
// PODINFO "NAME.POD" — print provenance, size, capabilities and NEED rationale
// without running anything (the transparency feature made concrete).
// ===========================================================================
static const struct { uint64_t bit; const char *name; } pod_cap_names[] = {
    { POD_CAP_CONSOLE,"CONSOLE" }, { POD_CAP_VARS,"VARS" }, { POD_CAP_FILES,"FILES" },
    { POD_CAP_DIRS,"DIRS" }, { POD_CAP_GRAPHICS,"GRAPHICS" }, { POD_CAP_SOUND,"SOUND" },
    { POD_CAP_GPIO,"GPIO" }, { POD_CAP_I2C,"I2C" }, { POD_CAP_TIME,"TIME" },
    { POD_CAP_HEAP,"HEAP" }, { POD_CAP_KEYWORD,"KEYWORD" }, { POD_CAP_SPAWN,"SPAWN" },
    { POD_CAP_RAWMEM,"RAWMEM" }, { POD_CAP_CORES,"CORES" }, { POD_CAP_NET,"NET" },
    { POD_CAP_DEBUG,"DEBUG" },
};

void pod_puts(const char *s) { int n = 0; while (s[n]) n++; con_putsn(s, n); }
void pod_line(const char *label, const char *val) {
    pod_puts(label); pod_puts(val); con_putc('\n');
}

void stmt_podinfo(void) {
    lex_next();                                         // consume PODINFO
    char path[96];
    if (!pod_arg_path(path, sizeof path)) return;
    pod_image_t im;
    if (!pod_open(path, &im, 0)) return;

    char buf[128];
    if (pod_mark_get(&im, "name", buf, sizeof buf)) pod_line("name : ", buf);
    if (pod_mark_get(&im, "vers", buf, sizeof buf)) pod_line("vers : ", buf);
    if (pod_mark_get(&im, "auth", buf, sizeof buf)) pod_line("auth : ", buf);
    if (pod_mark_get(&im, "date", buf, sizeof buf)) pod_line("date : ", buf);
    if (pod_mark_get(&im, "tool", buf, sizeof buf)) pod_line("tool : ", buf);
    if (pod_mark_get(&im, "desc", buf, sizeof buf)) pod_line("desc : ", buf);
    pod_line("kind : ", (im.flags & POD_KIND_EXTENSION) ? "extension" : "program");

    char num[32];
    pod_puts("size : "); int nl = dbl_to_str(num, (double)im.image_size); con_putsn(num, nl);
    pod_puts(" bytes\n");

    pod_puts("caps : ");
    int first = 1;
    for (unsigned i = 0; i < sizeof pod_cap_names / sizeof pod_cap_names[0]; i++)
        if (im.caps & pod_cap_names[i].bit) {
            if (!first) pod_puts(", ");
            pod_puts(pod_cap_names[i].name); first = 0;
        }
    if (first) pod_puts("(none)");
    con_putc('\n');

    if (im.need) {                                      // one "CAP=why" line each
        const unsigned char *p = im.need, *end = p + im.need_len;
        while (p < end) {
            const unsigned char *r = p; int rl = 0;
            while (p < end && *p) { p++; rl++; }
            pod_puts("need : "); con_putsn((const char *)r, rl); con_putc('\n');
            p++;
        }
    }
}

// = PODCAPS("NAME.POD") — the capability bitmask, for a program that wants to
// check before running. Current token is PODCAPS.
value_t eval_podcaps(void) {
    lex_next();                                         // consume PODCAPS
    if (!expect(T_LP)) return v_num(0);
    char path[96];
    if (!pod_expr_path(path, sizeof path)) return v_num(0);
    if (!expect(T_RP)) return v_num(0);
    pod_image_t im;
    if (!pod_open(path, &im, 0)) return v_num(0);
    return v_num((double)im.caps);
}
