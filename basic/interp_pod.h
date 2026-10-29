#ifndef INTERP_POD_H
#define INTERP_POD_H

#include "interp_types.h"

/* ==================================================================
 * interp_pod.c -- POD executables: the loader and RUN/PODLOAD/PODINFO/... .
 * ================================================================== */

/* -------------------------------------------------------------- globals */

extern double g_pod_status;   // exit status of the last program POD
extern char        pod_argbuf[LINE_LEN + 16];
extern const char *pod_argv[40];
extern unsigned char pod_filebuf[POD_SLOT_SIZE];   // whole-file staging for one load
extern pod_slot_t pod_slots[POD_MAX];

/* ------------------------------------------------------------ functions */

// --- CRC-32C (Castagnoli), matching the writer and the Cortex-A72 crc32cx ---
uint32_t pod_crc32c (const unsigned char *p, int n);

// Little-endian readers (the file is LE; AArch64 is -mstrict-align so read bytewise).
uint16_t pod_rd16 (const unsigned char *p);

// POD loader/runtime helper: rd32.
uint32_t pod_rd32 (const unsigned char *p);

// POD loader/runtime helper: rd64.
uint64_t pod_rd64 (const unsigned char *p);

// POD loader/runtime helper: wr32.
void pod_wr32 (unsigned char *p, uint32_t v);

// POD loader/runtime helper: wr64.
void pod_wr64 (unsigned char *p, uint64_t v);

// POD loader/runtime helper: dn puts.
void pod_dn_puts (const char *a, int b);

// POD loader/runtime helper: dn putc.
void pod_dn_putc (int a);

// POD loader/runtime helper: dn geti.
int pod_dn_geti (void);

// POD loader/runtime helper: dn int i.
int pod_dn_int_i (int a);

// POD loader/runtime helper: dn getnum.
int pod_dn_getnum (const char *a, double *b);

// POD loader/runtime helper: dn setnum.
void pod_dn_setnum (const char *a, double b);

// POD loader/runtime helper: dn getstr.
int pod_dn_getstr (const char *a, char *b, int c);

// POD loader/runtime helper: dn setstr.
void pod_dn_setstr (const char *a, const char *b, int c);

// POD loader/runtime helper: dn fopen.
int pod_dn_fopen (const char *a, int b);

// POD loader/runtime helper: dn fread.
int pod_dn_fread (int a, void *b, int c);

// POD loader/runtime helper: dn fwrite.
int pod_dn_fwrite (int a, const void *b, int c);

// POD loader/runtime helper: dn fseek.
long int pod_dn_fseek (int a, long int b, int c);

// POD loader/runtime helper: dn clear.
void pod_dn_clear (uint32_t a);

// POD loader/runtime helper: dn pixel.
void pod_dn_pixel (int a, int b, uint32_t c);

// POD loader/runtime helper: dn rect.
void pod_dn_rect (int a, int b, int c, int d, uint32_t e);

// POD loader/runtime helper: dn gmode.
int pod_dn_gmode (int a, int b, int c);

// POD loader/runtime helper: dn gwrite.
void pod_dn_gwrite (int a, int b);

// POD loader/runtime helper: dn timecs.
uint32_t pod_dn_timecs (void);

// POD loader/runtime helper: dn alloc.
void *pod_dn_alloc (unsigned int a);

// POD loader/runtime helper: dn free.
void pod_dn_free (void *a);

// POD loader/runtime helper: dn realloc.
void *pod_dn_realloc (void *a, unsigned int b);

// POD loader/runtime helper: dn alloca.
void *pod_dn_alloca (unsigned int a, unsigned int b);

// POD loader/runtime helper: dn icache.
void pod_dn_icache (const void *a, long unsigned int b);

// POD loader/runtime helper: dn blit8.
void pod_dn_blit8 (const unsigned char *i, const unsigned int *p, int w, int h, int dx, int dy, int s);

// POD loader/runtime helper: dn remove.
int pod_dn_remove (const char *a);

// POD loader/runtime helper: dn spawn.
int pod_dn_spawn (const char *a, int b, const char *const *c);

// POD loader/runtime helper: dn mkdir.
int pod_dn_mkdir (const char *a);

// POD loader/runtime helper: dn diropen.
int pod_dn_diropen (const char *a);

// POD loader/runtime helper: dn dirread.
int pod_dn_dirread (char *a, int b, int *c, long int *d);

// POD loader/runtime helper: dn getcwd.
int pod_dn_getcwd (char *a, int b);

// BerryServices/POD wrapper: mkdir.
int svc_pod_mkdir (const char *path);

// --- refusal stubs for the ABI v4 surface (ungranted -> no-op / harmless) ---
double *pod_dn_numarr (const char *a, int *b);

// POD loader/runtime helper: dn recarr.
double *pod_dn_recarr (const char *a, int *b, int *c);

// POD loader/runtime helper: dn recfield.
int pod_dn_recfield (const char *a, const char *b);

// POD loader/runtime helper: dn recget.
int pod_dn_recget (const char *a, int b, const char *c, char *d, int e);

// POD loader/runtime helper: dn recset.
void pod_dn_recset (const char *a, int b, const char *c, const char *d, int e);

// POD loader/runtime helper: dn longi.
long int pod_dn_longi (int a);

// POD loader/runtime helper: dn ii.
int pod_dn_ii (int a, int b);

// POD loader/runtime helper: dn mouse.
void pod_dn_mouse (int *a, int *b, int *c);

// POD loader/runtime helper: dn keysdown.
int pod_dn_keysdown (int *a, int b);

// POD loader/runtime helper: dn aopen.
int pod_dn_aopen (int r);

// POD loader/runtime helper: dn geti0.
int pod_dn_geti0 (void);

// POD loader/runtime helper: dn awrite.
int pod_dn_awrite (const short int *s, int f);

// POD loader/runtime helper: dn aclose.
void pod_dn_aclose (void);

// POD loader/runtime helper: dn scrsz.
void pod_dn_scrsz (int *a, int *b);

// POD loader/runtime helper: dn glyph.
void pod_dn_glyph (int a, int b, int c, uint32_t d, uint32_t e);

// POD loader/runtime helper: dn getpix.
uint32_t pod_dn_getpix (int a, int b);

// POD loader/runtime helper: dn circ.
void pod_dn_circ (int a, int b, int c, uint32_t d);

// POD loader/runtime helper: dn fpoly.
void pod_dn_fpoly (const int *a, int b, uint32_t c);

// POD loader/runtime helper: dn clip4.
void pod_dn_clip4 (int a, int b, int c, int d);

// POD loader/runtime helper: dn void.
void pod_dn_void (void);

// POD loader/runtime helper: dn iii.
void pod_dn_iii (int a, int b, int c);

// POD loader/runtime helper: dn pline.
void pod_dn_pline (const int *a, int b, int c, uint32_t d);

// POD loader/runtime helper: dn fontload.
int pod_dn_fontload (const char *a);

// POD loader/runtime helper: dn gtext.
void pod_dn_gtext (int a, int b, const char *c, int d, uint32_t e);

// Fill a services table: every slot a refusal stub, then the granted groups
// overwritten with the real service callbacks (defined in interp_seed.inc).
void pod_build_svc (BerryServices *s, uint64_t caps);

// Read a POD file into pod_filebuf and verify it end to end (magic, header CRC,
// declared file_size, every chunk CRC, payload CRC and the SEAL). Fills *im.
// Returns 1 on success; on failure raises a BASIC error and returns 0. With
// silent_missing set, a not-found file returns -1 WITHOUT raising, so a caller
// probing for an optional file (the /sys command dispatch) can fall through.
int pod_open (const char *path, pod_image_t *im, int silent_missing);

// Copy a verified image into slot `si`, apply relocations, make it executable,
// and build its services table. Returns the image base pointer.
unsigned char *pod_instantiate (int si, const pod_image_t *im);

// POD loader/runtime helper: free slot.
int pod_free_slot (void);

// The MARK value for a key (e.g. "name"), copied NUL-terminated into out. MARK
// is a run of NUL-terminated "key=value" records. Returns 1 if found.
int pod_mark_get (const pod_image_t *im, const char *key, char *out, int outsz);

// PODLOAD/PODINFO/RUN "NAME.POD": a quoted or bare path, defaulting to .POD. These
// are typed as commands, so a bare name (PODINFO HELLO) is the norm. The keyword
// is already consumed and the path is the current token, so rewind the cursor to
// its start and read it raw.
int pod_arg_path (char *path, int pathsz);

// Same .POD default, but from a string *expression* - for PODCAPS(...), which is
// a value function used inside expressions where a variable (PODCAPS(f$)) or a
// built path must still evaluate rather than be taken literally.
int pod_expr_path (char *path, int pathsz);

// Split a raw command tail into argv[1..], with argv[0] set to `name`. Tokens are
// whitespace-separated; a "double-quoted" run is one token with the quotes
// stripped (so file names with spaces survive). Returns argc.
int pod_split_args (const char *name, const char *raw);

// Instantiate an already-verified program POD in a transient slot, run its
// pod_main(svc, argc, argv), free the slot, and record the exit status.
void pod_exec_program (pod_image_t *im, int argc, const char *const *argv);

// The CAP_SPAWN service: run a program POD nested inside another (a build tool
// spawning the compiler). Returns the spawned POD's exit status, or a negative
// code on failure. It saves/restores g_err so a load failure is reported to the
// caller as a return value rather than aborting the BASIC program; the spawned
// POD runs in its own slot with its own capability-gated table.
int pod_spawn (const char *path, int argc, const char *const *argv);

// BerryServices/POD wrapper: spawn.
int svc_pod_spawn (const char *path, int argc, const char *const *argv);

// basename of a path (after the last '/'), minus any extension, into out.
void pod_basename (const char *path, char *out, int outsz);

// RUN "NAME.POD" [, args$] — load and execute a program POD.
void pod_run_program (void);

// The /sys command shell: a bare word (or RUN word) whose name matches a POD in
// /sys is run as a command, the rest of the line passed as its arguments. This
// is what makes the machine command-driven: `tcc -pod hello.c -o HELLO.POD`
// finds /sys/TCC.POD and runs it with argv = { "tcc", "-pod", ... }.
//
// `name` is the command word (already the identifier the lexer read); `rawtail`
// is the raw text after it, up to end of line. Returns 1 if it was a /sys
// command (ran, or raised a real error); 0 if there is no such command (so the
// caller falls back to treating the word as a variable).
int sys_try_command (const char *name, const char *rawtail);

// Execute the PODLOAD statement.
void stmt_podload (void);

// Remove every keyword owned by slot `si` from the shared table (compacting it).
void pod_unregister_keywords (int si);

// PODFREE "NAME" — unload a resident extension POD by its (MARK) name.
void stmt_podfree (void);

// Drop every loaded POD and its keywords (called on NEW).
void pod_reset (void);

// POD loader/runtime helper: puts.
void pod_puts (const char *s);

// POD loader/runtime helper: line.
void pod_line (const char *label, const char *val);

// Execute the PODINFO statement.
void stmt_podinfo (void);

// = PODCAPS("NAME.POD") — the capability bitmask, for a program that wants to
// check before running. Current token is PODCAPS.
value_t eval_podcaps (void);

#endif /* INTERP_POD_H */
