#ifndef INTERP_PARSE_H
#define INTERP_PARSE_H

#include "interp_types.h"

/* ==================================================================
 * interp_parse.c -- Evaluator support helpers shared by the expression evaluator.
 * ================================================================== */

/* -------------------------------------------------------------- globals */

extern int        g_dir_valid;
extern stg_dirent g_dirent;
extern double rnd_last;
extern unsigned long rnd_seed;
extern long long time_base;   // TIME (centiseconds) = con_micros()/10000 - time_base

/* ------------------------------------------------------------ functions */

// Pull a required numeric/string operand, raising TYPE MISMATCH otherwise.
double need_num (void);

// Need str.
value_t need_str (void);

// Human-readable name of a token type, for "Expected X" error messages.
const char *tok_name (int t);

// Expect.
int expect (int t);

// Str in scratch.
value_t str_in_scratch (const char *src, int len);

// Parse "(e1[,e2,...])" into subs[], with tok currently at '('. Returns count.
int parse_subscripts (int *subs, int *nsub);

// Resolve an array element reference (tok at '('): parse subscripts, auto-DIM
// to 0..10 per dimension if the array does not exist, and return the pool index
// in *idx and the array in *out.
int arr_elem (const char *name, int is_str, int *idx, arr_t **out);

// Read a type or field name into `out` and consume it. These names live in
// their own namespace and only ever appear where nothing else is legal (after
// TYPE or AS, or in a field list), so a word that happens to be a keyword -
// POINT, SIZE, TIME - is still a usable name. Field *reads* never come through
// here: the lexer hands those over as T_LABEL, which is never keyword-matched,
// so accepting keywords here is what keeps `TYPE point : size` and `p.size`
// agreeing on the same name. Returns 0 on error, having raised `what`.
int read_name_word (char *out, const char *what);

// Parse the element selector of a record reference: '(index)' for an array of
// records, nothing at all for a scalar one. On entry the current token is just
// past the variable's name. Returns 0 on error, having raised it.
int rec_index (const var_t *v, int *e);

// Resolve a field reference. On entry `v` is the record variable and the current
// token is whatever followed its name: an optional '(index)' selecting an
// element, then the '.field' (a T_LABEL - see the lexer). On return the current
// token is past the field name. Returns 0 on error, having raised it.
int rec_field_ref (const var_t *v, fieldref_t *fr);

// Read a record field's current value.
value_t rec_field_get (const var_t *v);

// Parse a target at the current token - name, name(subs), p.field, e(3).field -
// and resolve where its value lives. Returns 0 on error, having raised it.
int parse_target (target_t *t);

// Target put num.
void target_put_num (const target_t *t, double x);

// Target put str.
void target_put_str (const target_t *t, const char *s, int len);

// Rnd float.
double rnd_float (void);

// Parse a single function argument as the next factor: a primary expression,
// which is either a parenthesised group or a bare value. This makes the
// single-argument math functions paren-optional, BBC-style (SQR 3 = SQR(3),
// SQR(3) and SQR(X+1) all work, and SQR binds tighter than the operators around
// it, so X * SQR 3 / 6 is X * (SQR 3) / 6).
double factor_num (void);

// Parse a file channel operand: the '#' prefix followed by a numeric factor (so
// BGET#ch+1 is (BGET#ch)+1, and BGET#(a+1) uses parentheses). Returns the channel.
int read_channel (void);

// Copy a BASIC string value into a NUL-terminated C filename buffer.
void copy_fname (value_t s, char *out, int outsz);

// Read a file/directory path starting at the lexer cursor `lx`, accepting either
// a quoted string ("my dir") or a bare word typed without quotes (examples, ..,
// build/PLOT.POD). Reading straight from the raw source is what lets a path's
// '.', '..' and '/' through: the expression tokeniser rejects a lone '.', so a
// bare relative path must never be lexed as an operand. `defext` (e.g. ".BAS")
// is appended when the name carries no '.', or pass 0 to keep the name exactly as
// typed (directories). A path is a literal here, not an expression, so use quotes
// only when the name contains spaces. Callers must position `lx` just after the
// keyword with the argument NOT yet lexed (do not lex_next first); a caller whose
// argument is already the current token rewinds with `lx = tok_start;` first.
int read_path (char *out, int outsz, const char *defext);

// Fmt u2.
void fmt_u2 (char *p, int v);

// Fmt date.
void fmt_date (char *b, int y, int m, int d);

// Fmt time.
void fmt_time (char *b, int h, int m);

#endif /* INTERP_PARSE_H */
