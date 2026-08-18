#ifndef INTERP_SEED_H
#define INTERP_SEED_H

#include "interp_types.h"

/* ==================================================================
 * interp_seed.c -- Native "seeds" (loadable AArch64 blobs), DICT/LIST/TREE collections,
 * and the BerryServices vtable handed to seeds and PODs.
 * ================================================================== */

/* -------------------------------------------------------------- globals */

extern coll_t colls[COLL_MAX];
extern int g_pending_key;
extern char g_seed_retstr[MAX_STR];   // string result staged by set_return_str
extern int  g_seed_retstr_len;   // -1 = the last call set no string
extern const BerryServices g_svc;
extern seed_blk seed_freelist;   // circular free-list sentinel
extern seed_blk *seed_freep;   // 0 until first use / after a reset
extern int        seed_kw_n;
extern seed_kw_t  seed_kw_tab[SEED_KW_MAX];
extern int         seed_rec_cap;   // allocated capacity
extern int         seed_rec_n;   // records touched (high-water within the array)
extern seed_rec_t *seed_recs;   // grown from the run heap; 0 after a reset
extern seed_blk *seed_sys_top;   // 0 until first use; else the persistent floor

/* ------------------------------------------------------------ functions */

// Drop every loaded seed. The blobs and this table live in the run heap, which
// seed_heap_reset() wipes on RUN/NEW, so we just forget the pointers.
void seed_recs_reset (void);

// Reserve a fresh record and return its index, or -1 on OOM. Reuses a freed slot
// when one exists, else appends (growing the array from the run heap).
int seed_rec_alloc (void);

// A page-aligned executable block from the run heap, sized to nbytes (rounded up
// to a page). Not individually freed: reclaimed wholesale by the RUN/NEW reset.
void *seed_alloc_page (unsigned int nbytes);

// Reset seed heap to its initial state.
void seed_heap_reset (void);

// Report the run heap's used/total bytes (for the F12 system overlay). Total is
// the run-heap region [seed_heap, seed_sys_top); free is the sum of the K&R
// free-list blocks (before first use the whole region is free). Sizes are in
// header units; ×sizeof(seed_blk) gives bytes.
void seed_heap_stats (long unsigned int *used, long unsigned int *total);

// Exported (see console.h): fill the interpreter's memory-pool usage for the
// kernel's F12 overlay. Non-static on purpose - this is the one symbol the
// kernel reaches into the interpreter for. Harmless (unused) on the host build.
void sys_meminfo (sysmem_t *m);

// Initialise seed heap.
void seed_heap_init (void);

// Reserve `nbytes` of page-aligned persistent memory from the top of the arena
// for a native keyword seed's code. Keyword seeds are scanned once at startup,
// BEFORE the run heap is first used, so moving the floor down here never disturbs
// a live allocation; if the run heap is already in use we refuse rather than
// corrupt it (return 0). Never individually freed: it lives for the session.
void *seed_persist_page (unsigned int nbytes);

// Native-seed support: alloc.
void *seed_alloc (unsigned int nbytes);

// Native-seed support: free.
void seed_free (void *ap);

// Usable payload bytes of an allocated block (its header records the size).
unsigned int seed_block_bytes (void *ap);

// realloc: grow or shrink a block, preserving its contents. A shrink (or a grow
// that already fits the rounded-up block) keeps the same pointer; otherwise a new
// block is allocated, the old bytes copied, and the old block freed. On failure
// the original block is left untouched and 0 is returned (standard semantics).
void *seed_realloc (void *ap, unsigned int nbytes);

// aligned allocation: return a block whose payload is `alignment`-aligned, still
// freeable with the ordinary free. Blocks are already 16-aligned, so smaller
// alignments are plain allocs; for larger ones we over-allocate, split off the
// unaligned prefix as its own free block, and hand back the aligned remainder
// (which carries a normal block header, so free/realloc work on it unchanged).
void *seed_alloc_aligned (unsigned int alignment, unsigned int nbytes);

// Collection value helper: clear.
void cval_clear (cval_t *c);

// Overwrite *c with the BASIC value v (a string is copied into the heap so it
// survives independently of BASIC's own GC heap). Returns 0 (and raises) on OOM.
int cval_store (cval_t *c, value_t v);

// Read a stored value as a typed BASIC value. The numeric/string variants raise
// a type mismatch if the stored value is the other kind (like A vs A$).
value_t cval_num (cval_t *c);

// Collection value helper: strv.
value_t cval_strv (cval_t *c);

// LIST collection helper: reserve.
int list_reserve (list_t *L, int need);

// LIST collection helper: ins.
int list_ins (list_t *L, int i, value_t v);

// LIST collection helper: del.
void list_del (list_t *L, int i);

// DICT collection helper: find.
int dict_find (dict_t *D, const char *key, int klen);

// DICT collection helper: set.
int dict_set (dict_t *D, const char *key, int klen, value_t v);

// DICT collection helper: del.
void dict_del (dict_t *D, const char *key, int klen);

// TREE collection helper: find.
tnode_t *tree_find (tree_t *T, double key);

// TREE collection helper: set.
int tree_set (tree_t *T, double key, value_t v);

// TREE collection helper: del.
void tree_del (tree_t *T, double key);

// TREE collection helper: edge.
tnode_t *tree_edge (tree_t *T, int rightmost);

// The idx-th node in ascending key order (0-based). Iterative in-order walk with
// an explicit heap stack, so a degenerate (deep) tree can't overflow the C stack.
tnode_t *tree_index (tree_t *T, int idx);

// Drop every handle. The objects' memory lives in the general heap, which is
// wiped by seed_heap_reset() on RUN/NEW, so we just clear the table alongside it.
void coll_reset (void);

// Collection registry: new.
double coll_new (int type, unsigned int objsize);

// Resolve a handle, requiring a particular type (0 = any). Raises on a bad or
// wrong-typed handle and returns 0.
void *coll_get (double h, int type);

// Collection registry: size of.
int coll_size_of (int type, void *o);

// BerryServices wrapper exposing "putc" to seeds and PODs.
void svc_putc (int c);

// BerryServices wrapper exposing "puts" to seeds and PODs.
void svc_puts (const char *s, int len);

// BerryServices wrapper exposing "vdu" to seeds and PODs.
void svc_vdu (int b);

// BerryServices wrapper exposing "screen_size" to seeds and PODs.
void svc_screen_size (int *c, int *r);

// BerryServices wrapper exposing "con_font" to seeds and PODs.
void svc_con_font (int *w, int *h);

// BerryServices wrapper exposing "con_glyph" to seeds and PODs.
void svc_con_glyph (int px, int py, int ch, uint32_t fg, uint32_t bg);

// BerryServices wrapper exposing "dir_open" to seeds and PODs.
int svc_dir_open (const char *path);

// BerryServices wrapper exposing "dir_read" to seeds and PODs.
int svc_dir_read (char *name, int namesz, int *is_dir, long int *size);

// BerryServices wrapper exposing "getcwd" to seeds and PODs.
int svc_getcwd (char *buf, int sz);

// BerryServices wrapper exposing "chdir" to seeds and PODs.
int svc_chdir (const char *path);

// BerryServices wrapper exposing "clip_set" to seeds and PODs.
void svc_clip_set (const char *d, int n);

// BerryServices wrapper exposing "clip_get" to seeds and PODs.
int svc_clip_get (char *b, int max);

// BerryServices wrapper exposing "clip_len" to seeds and PODs.
int svc_clip_len (void);

// BerryServices wrapper exposing "icache_sync" to seeds and PODs.
void svc_icache_sync (const void *a, long unsigned int n);

// BerryServices wrapper exposing "getkey" to seeds and PODs.
int svc_getkey (void);

// BerryServices wrapper exposing "inkey" to seeds and PODs.
int svc_inkey (int cs);

// The call is the poll: nothing services the mouse while a seed runs, so a seed
// wanting a live pointer calls this in its own loop. Raw framebuffer pixels,
// exactly as BASIC's MOUSEX/MOUSEY and the gfx_* drawing calls see them.
void svc_mouse (int *x, int *y, int *b);

// Double buffering: the same back buffer (and the same on/off state) BASIC's
// BUFFER/FLIP use, so a seed and a program can't each have their own idea of it.
int svc_gfx_backbuffer (int on);

// BerryServices wrapper exposing "gfx_flip" to seeds and PODs.
void svc_gfx_flip (void);

// BerryServices wrapper exposing "gfx_buffered" to seeds and PODs.
int svc_gfx_buffered (void);

// Modifier/lock state: not a key, so getkey/inkey can never carry it.
int svc_keymods (void);

// BerryServices wrapper exposing "keys_down" to seeds and PODs.
int svc_keys_down (int *out, int max);

// BerryServices wrapper exposing "audio_open" to seeds and PODs.
int svc_audio_open (int rate);

// BerryServices wrapper exposing "audio_avail" to seeds and PODs.
int svc_audio_avail (void);

// BerryServices wrapper exposing "audio_write" to seeds and PODs.
int svc_audio_write (const short int *s, int f);

// BerryServices wrapper exposing "audio_close" to seeds and PODs.
void svc_audio_close (void);

// The BASIC graphics mode (1 or 2). gfx_* drawing is always device pixels; this
// lets a seed convert coordinates BASIC passed in its current mode.
int svc_gfx_mode (void);

// Bas getkey.
int bas_getkey (void);

// Bas inkey.
int bas_inkey (int cs);

// A record variable has no scalar value of its own, so it is not a number and
// not a string: these four report "not found" for one rather than handing back
// the unused num/s members. Records are reached through the rec_* services.
int svc_get_num (const char *name, double *out);

// BerryServices wrapper exposing "set_num" to seeds and PODs.
void svc_set_num (const char *name, double val);

// BerryServices wrapper exposing "get_str" to seeds and PODs.
int svc_get_str (const char *name, char *buf, int buflen);

// BerryServices wrapper exposing "set_str" to seeds and PODs.
void svc_set_str (const char *name, const char *buf, int len);

// BerryServices wrapper exposing "num_array" to seeds and PODs.
double *svc_num_array (const char *name, int *out_len);

// BerryServices wrapper exposing "rec_find" to seeds and PODs.
const var_t *svc_rec_find (const char *name);

// BerryServices wrapper exposing "rec_array" to seeds and PODs.
double *svc_rec_array (const char *name, int *nelem, int *stride);

// BerryServices wrapper exposing "rec_field" to seeds and PODs.
int svc_rec_field (const char *name, const char *field);

// Resolve a text field to its descriptor; 0 if the record/element/field is not
// one. Shared by the two string services below.
strdesc_t *svc_rec_str_slot (const char *name, int elem, const char *field);

// BerryServices wrapper exposing "rec_get_str" to seeds and PODs.
int svc_rec_get_str (const char *name, int elem, const char *field, char *buf, int buflen);

// BerryServices wrapper exposing "rec_set_str" to seeds and PODs.
void svc_rec_set_str (const char *name, int elem, const char *field, const char *buf, int len);

// BerryServices wrapper exposing "set_return_str" to seeds and PODs.
void svc_set_return_str (const char *buf, int len);

// BerryServices wrapper exposing "time_cs" to seeds and PODs.
uint32_t svc_time_cs (void);

// BerryServices wrapper exposing "alloc" to seeds and PODs.
void *svc_alloc (unsigned int nbytes);

// BerryServices wrapper exposing "free" to seeds and PODs.
void svc_free (void *ptr);

// BerryServices wrapper exposing "realloc" to seeds and PODs.
void *svc_realloc (void *ptr, unsigned int nbytes);

// BerryServices wrapper exposing "alloc_aligned" to seeds and PODs.
void *svc_alloc_aligned (unsigned int a, unsigned int n);

// GPIO passthroughs (see gpio.h). The driver validates the pin range itself, so
// these are thin; on the host build every gpio_* is a stub and gpio_available()
// is 0, which a seed can test via svc->gpio_avail().
int svc_gpio_avail (void);

// BerryServices wrapper exposing "gpio_mode" to seeds and PODs.
int svc_gpio_mode (int pin, int mode, int alt);

// BerryServices wrapper exposing "gpio_pull" to seeds and PODs.
int svc_gpio_pull (int pin, int pull);

// BerryServices wrapper exposing "gpio_write" to seeds and PODs.
void svc_gpio_write (int pin, int level);

// BerryServices wrapper exposing "gpio_read" to seeds and PODs.
int svc_gpio_read (int pin);

// BerryServices wrapper exposing "gpio_set" to seeds and PODs.
void svc_gpio_set (uint32_t mask);

// BerryServices wrapper exposing "gpio_clr" to seeds and PODs.
void svc_gpio_clr (uint32_t mask);

// BerryServices wrapper exposing "gpio_level" to seeds and PODs.
uint32_t svc_gpio_level (void);

// BerryServices wrapper exposing "gpio_wait" to seeds and PODs.
int svc_gpio_wait (int pin, int edge, int cs);

// SD-card files: thin adapters over the storage channel API (see storage.h),
// which the seed <stdio.h> is built on. All of this shares the file channels and
// filesystem (long names included) with BASIC's OPENIN/OPENOUT.
int svc_file_open (const char *name, int mode);

// BerryServices wrapper exposing "file_close" to seeds and PODs.
int svc_file_close (int fh);

// BerryServices wrapper exposing "file_read" to seeds and PODs.
int svc_file_read (int fh, void *buf, int n);

// BerryServices wrapper exposing "file_write" to seeds and PODs.
int svc_file_write (int fh, const void *buf, int n);

// BerryServices wrapper exposing "file_seek" to seeds and PODs.
long int svc_file_seek (int fh, long int off, int whence);

// BerryServices wrapper exposing "file_size" to seeds and PODs.
long int svc_file_size (int fh);

// BerryServices wrapper exposing "file_eof" to seeds and PODs.
int svc_file_eof (int fh);

// BerryServices wrapper exposing "file_remove" to seeds and PODs.
int svc_file_remove (const char *name);

// Format a double exactly as PRINT/STR$ do, so a seed's printf %f matches BASIC.
int svc_fmt_num (double v, char *out);

// Device-pixel drawing forwards to gfx.h (framebuffer target / host no-op);
// font management reuses the same ttf.h engine as BASIC's GTEXT.
int svc_gfx_avail (void);

// BerryServices wrapper exposing "gfx_width" to seeds and PODs.
int svc_gfx_width (void);

// BerryServices wrapper exposing "gfx_height" to seeds and PODs.
int svc_gfx_height (void);

// BerryServices wrapper exposing "gfx_clear" to seeds and PODs.
void svc_gfx_clear (uint32_t rgb);

// BerryServices wrapper exposing "gfx_putpixel" to seeds and PODs.
void svc_gfx_putpixel (int x, int y, uint32_t rgb);

// BerryServices wrapper exposing "gfx_getpixel" to seeds and PODs.
uint32_t svc_gfx_getpixel (int x, int y);

// BerryServices wrapper exposing "gfx_line" to seeds and PODs.
void svc_gfx_line (int x1, int y1, int x2, int y2, uint32_t rgb);

// BerryServices wrapper exposing "gfx_fillrect" to seeds and PODs.
void svc_gfx_fillrect (int x1, int y1, int x2, int y2, uint32_t rgb);

// BerryServices wrapper exposing "gfx_blit8" to seeds and PODs.
void svc_gfx_blit8 (const unsigned char *idx, const unsigned int *pal, int w, int h, int dx, int dy, int scale);

// BerryServices wrapper exposing "gfx_circle" to seeds and PODs.
void svc_gfx_circle (int cx, int cy, int r, uint32_t rgb);

// BerryServices wrapper exposing "gfx_fillcircle" to seeds and PODs.
void svc_gfx_fillcircle (int cx, int cy, int r, uint32_t rgb);

// BerryServices wrapper exposing "gfx_ellipse" to seeds and PODs.
void svc_gfx_ellipse (int cx, int cy, int rx, int ry, uint32_t rgb);

// BerryServices wrapper exposing "gfx_fillellipse" to seeds and PODs.
void svc_gfx_fillellipse (int cx, int cy, int rx, int ry, uint32_t rgb);

// BerryServices wrapper exposing "gfx_fillpoly" to seeds and PODs.
void svc_gfx_fillpoly (const int *xy, int npts, uint32_t rgb);

// BerryServices wrapper exposing "gfx_line_style" to seeds and PODs.
void svc_gfx_line_style (int w, int j, int c);

// BerryServices wrapper exposing "gfx_polyline" to seeds and PODs.
void svc_gfx_polyline (const int *pts, int n, int cl, uint32_t rgb);

// BerryServices wrapper exposing "gfx_flood" to seeds and PODs.
void svc_gfx_flood (int x, int y, uint32_t rgb);

// BerryServices wrapper exposing "gfx_clip" to seeds and PODs.
void svc_gfx_clip (int x1, int y1, int x2, int y2);

// BerryServices wrapper exposing "gfx_noclip" to seeds and PODs.
void svc_gfx_noclip (void);

// BerryServices wrapper exposing "font_load" to seeds and PODs.
int svc_font_load (const char *name);

// BerryServices wrapper exposing "font_select" to seeds and PODs.
int svc_font_select (int handle);

// BerryServices wrapper exposing "font_size" to seeds and PODs.
void svc_font_size (int px);

// BerryServices wrapper exposing "font_style" to seeds and PODs.
void svc_font_style (int b, int i, int u);

// BerryServices wrapper exposing "gfx_text" to seeds and PODs.
void svc_gfx_text (int x, int y, const char *s, int len, uint32_t rgb);

// BerryServices wrapper exposing "text_width" to seeds and PODs.
int svc_text_width (const char *s, int len);

// BerryServices wrapper exposing "text_height" to seeds and PODs.
int svc_text_height (void);

// spawn/mkdir round out the unified ABI. A seed cannot launch another POD (that
// path needs the loader context the /sys shell has), so spawn is refused with
// -1; mkdir maps straight to storage, which the interpreter fully supports.
int svc_spawn (const char *path, int argc, const char *const *argv);

// BerryServices wrapper exposing "mkdir" to seeds and PODs.
int svc_mkdir (const char *path);

/* run_basic: run a BASIC program synchronously, on the visible screen, returning
 * when it stops - so a tool like the editor can repaint itself afterwards (its
 * POD state stays live in memory the whole time). This mirrors a BASIC
 * `RUN "file"`: the caller is a POD dispatched by pod_run_program, which reads no
 * more tokens once it returns, so re-entering the run loop here is safe. g_err is
 * saved/restored so the program's own error (already reported by the run) does
 * not leak into the caller's immediate-mode context. */
int  load_bas_file(const char *name);                /* interp_files.inc   */
void run_program_once(int start_pc, int start_off);  /* interp_control.inc */
int svc_run_basic (const char *path);

// Parse "handle [, arg ...]" (tok at the handle, stops on the first non-comma
// token), invoke the seed, and return its numeric result. String arguments are
// snapshotted into scratch so they stay valid even if the seed triggers GC by
// writing a variable. g_seed_retstr[_len] receives any string result.
double seed_run_collect (void);

// Find a registered keyword by (already upper-cased) name; -1 if none.
int seed_kw_lookup (const char *name);

// The spelling of a seed's keyword, by registry index. The counterpart of
// kw_spelling() for built-ins: a type or field may be named after a keyword,
// and a seed-registered one is no different (a seed adding PARTICLE must not
// stop you writing TYPE particle).
const char *seed_kw_name (int idx);

// Bytes to reserve for a loaded seed: the header's image_size (total footprint
// including .bss, stamped by tcc -seed) when it is known, else the blob length
// plus a 16 KB zeroed margin for a gcc-built seed's .bss (matching the old fixed
// slot, which zeroed 16 KB). Rounded up to a page.
unsigned int seed_footprint (const struct seed_header *hdr, int len);

// Validate a loaded .sed blob and, if it registers a keyword, copy it into a
// persistent code block and add it to the table. Returns 1 if a keyword was
// installed.
int seed_kw_register (const char *blob, int len);

// Snapshot one evaluated value into a berry_arg (strings copied to GC-stable
// scratch, valid for the duration of the call).
int seed_fill_arg (berry_arg *a, value_t v);

// Gather the argument list for a seed keyword. paren=1 reads NAME(a, b, ...);
// paren=0 reads the bare statement form NAME a, b, ... up to end of statement.
// Returns the count, or -1 on error (with g_err set).
int seed_gather_args (berry_arg *argv, int max, int paren);

// Invoke a registered keyword's seed with the gathered args. Returns its numeric
// result in *out (string results are staged in g_seed_retstr). 0 ok, -1 on error.
int seed_kw_invoke (seed_kw_t *k, berry_arg *argv, int argc, double *out);

// Function form: x = NAME(args) / a$ = NAME$(args). Current token is the keyword.
value_t eval_seed_keyword (int id);

// Statement form: NAME arg, arg. Current token is the keyword; result discarded.
void exec_seed_keyword (int id);

#endif /* INTERP_SEED_H */
