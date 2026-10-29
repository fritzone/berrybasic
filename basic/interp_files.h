#ifndef INTERP_FILES_H
#define INTERP_FILES_H

#include "interp_types.h"

/* ==================================================================
 * interp_files.c -- Storage commands (LOAD/SAVE/CAT/...), modules (IMPORT), and the editor.
 * ================================================================== */

/* -------------------------------------------------------------- globals */

extern char chain_q[CHAIN_MAX][64];
extern int  chain_qn;
extern int  g_auto_active;   // AUTO mode: auto-number each entered line
extern int  g_auto_num;   // next line number to offer
extern int  g_auto_step;
extern char g_prefill[LINE_LEN];   // one-shot prefill for the next line (EDIT)
extern int  g_prefill_len;
extern int g_renum_start, g_renum_step;
extern int g_renum_start, g_renum_step;
extern char imported_names[MAX_MODULES][64];
extern int  n_imported;
extern char stg_buf[STG_BUF_SIZE];

/* ------------------------------------------------------------ functions */

// Map a storage error code to a BASIC error message.
void stg_err (int code);

// LOAD/SAVE/DELETE filename: quoted or bare (LOAD WELCOME finds WELCOME.BAS via
// the .BAS default). Thin wrapper over read_path; callers must not lex past the
// keyword first, so lx still points just after LOAD/SAVE/DELETE.
int read_filename (char *out, int outsz);

// Uint to str.
int uint_to_str (char *p, int v);

// SAVE "name" : serialise the program ("<num> <text>\n" per line) and write it.
void stmt_save (void);

// Read a .BAS file into the program store, replacing the current program (and
// clearing variables), exactly as LOAD does. Returns 0 or a negative stg_* code.
// Shared by LOAD and CHAIN.
int load_bas_file (const char *name);

// LOAD "name" : clear the current program and parse the file's numbered lines.
void stmt_load (void);

// Reset chain to its initial state.
void chain_reset (void);

// Chain enqueue.
void chain_enqueue (const char *name);

// Chain dequeue.
void chain_dequeue (char *out);

// Uppercase a program name and default its extension to .BAS (the FAT is 8.3 and
// case-folding, so "koch" and "graphics/koch" both resolve after this).
void chain_normalise (char *name);

// CHAIN callee$ [, caller$]
void stmt_chain (void);

// LIST "file" support (the parser is in interp_list.inc). Splits a raw file line
// "NUM TEXT" into *num and a pointer to TEXT (past the number and one space);
// returns the whole line with *num = 0 when there is no leading number.
const char *list_line_split (const char *line, int *num);

// Pause the listing when the screen fills (target only; con_rows()==0 on host and
// in tests, so page==0 and nothing ever blocks). Returns 1 if the user quit.
int list_page (int printed, int page);

// LIST [SIMPLE] "file": list a program file from the card. Pretty form mirrors
// the in-memory LIST (gutter-aligned numbers, block indentation, syntax colour);
// SIMPLE prints the file's lines verbatim. Both page when longer than the screen.
//
// Copy the next '\n'/'\r'-terminated line of buf[*i..n) into out (NUL-terminated,
// clamped to LINE_LEN), advancing *i past the line and its terminator. Returns 0
// at end of buffer.
int list_next_line (const char *buf, int n, int *i, char *out);

// List file.
void list_file (const char *name, int simple);

// Append one line to `module`'s block, keeping that block sorted by line number.
// Module blocks sit at the tail of prog[] (after main and earlier modules).
void module_add_line (int module, int num, const char *text);

// Load a module file and append its numbered lines under `module`. Returns 0, or
// a negative STG_* error if the file could not be read.
int load_module (const char *name, int module);

// If `text` begins with IMPORT "name", copy the (.BAS-defaulted) file name into
// `out` and return 1; otherwise 0. Uses the global lexer, safe in the pre-pass.
int line_import_name (const char *text, char *out, int outsz);

// Pre-pass, run before a program executes: pull in every module reachable via
// IMPORT and append its lines. prog_n grows as modules are added, so the loop
// also visits IMPORT lines inside modules (transitive). Dedup by name breaks
// cycles and avoids importing the same module twice.
void import_modules (void);

// Execute the DELETE statement.
void stmt_delete (void);

// Categorise a file by extension into a short icon tag and a colour (BBC index).
void cat_kind (const char *name, int is_dir, const char **icon, int *colour);

// Cat u to str.
int cat_u_to_str (char *out, long int v);

// Human-readable size: "845", "12.3K", "4.0M".
void cat_size_str (long int sz, char *out);

// Cat pad.
void cat_pad (int n);

// Cat 2d.
void cat_2d (int v);

// CAT / DIR : list the current directory. By default it is rich - a coloured
// type icon per file, the name, a human-readable size, and the date/time, in
// aligned columns, pausing for a keypress when the screen fills. CAT SIMPLE gives
// the plain one-name-per-line listing (and is what a program should use).
// Plain one-name-per-line listing of a directory (CAT SIMPLE "path"). Same shape
// as the backend stg_dir(), but works for any directory through the path-aware
// scan cursor rather than only the current one.
void cat_simple (const char *path);

// CAT [SIMPLE] ["path"] : list a directory (the current one by default). The rich
// form shows coloured type icons, sizes and dates with screen paging; SIMPLE
// prints just the names. An optional path lets you peek into a subdirectory
// (e.g. CAT "SEED") without CD-ing into it.
void stmt_cat (void);

// Directory commands take a quoted path (no ".BAS" default): MKDIR/CD/RMDIR
// "name", and PWD prints the current directory.
// lx sits just after the keyword here; read_path reads the path raw from there,
// so a bare "cd ..", "cd /sys" or "cd examples" needs no quotes. Do NOT lex_next
// first: a leading '.' (as in "..") is not a valid expression token.
void stmt_mkdir (void);

// Execute the RMDIR statement.
void stmt_rmdir (void);

// Execute the CD statement.
void stmt_cd (void);

// Execute the PWD statement.
void stmt_pwd (void);

// BPUT# ch, value : write a byte (numeric) or a whole string's bytes to a channel.
void stmt_bput (void);

// CLOSE# ch : close one channel; CLOSE# 0 closes every open channel.
void stmt_close (void);

// Scan the /seed directory once at startup and register every *keyword* seed
// (one built with SEED_KEYWORD, carrying a seed_keyword descriptor) so its
// keyword is part of the language immediately — used directly, without SEED/CALL.
// Plain SEED_EXPORT seeds are skipped here; they load on demand via SEED. Safe to
// read files mid-scan: stg_read resolves paths with its own buffers and does not
// touch the directory-enumeration cursor.
void seed_scan_keywords (void);

// SEED h%, "FILE.SED" : load a native seed from storage into an executable slot
// and put its handle into the numeric variable h%.
void stmt_seed (void);

// CALL h%, arg... : invoke a seed for its side effects, discarding the result.
// (CALL(...) and CALL$(...) as functions return the numeric / string result.)
void stmt_call (void);

// Map an old line number to its new one. References to a line that does not
// exist are left unchanged.
int remap_line (int old);

// Rewrite one line's text into `out`, remapping the line-number references that
// follow GOTO / GOSUB / RESTORE / THEN / ELSE (including the comma lists of
// ON ... GOTO / ON ... GOSUB). Numeric literals in expressions are left alone.
void renum_fixup_line (const char *in, char *out);

// RENUMBER [start][,step] : renumber the program (default 10,10) and fix up all
// line-number references so GOTO/GOSUB/RESTORE/THEN/ELSE/ON still point correctly.
void stmt_renumber (void);

// AUTO [start][,step] : enter auto line-numbering. The REPL offers each line
// number for editing; pressing Return on an empty line leaves AUTO.
void stmt_auto (void);

// EDIT n : recall line n into the input line, ready to edit and re-enter.
void stmt_edit (void);

#endif /* INTERP_FILES_H */
