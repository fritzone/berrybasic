#ifndef INTERP_CALL_H
#define INTERP_CALL_H

#include "interp_types.h"

/* ==================================================================
 * interp_call.c -- PROC / FN definition scanning and calling.
 * ================================================================== */

/* ------------------------------------------------------------ functions */

// Look ahead, from just past a record variable's name, to decide whether this
// argument is the whole record (p, e(3)) or an expression reading one of its
// fields (p.x). Only scans tokens - nothing is evaluated, so an index with a
// side effect can't fire twice - and puts the lexer back where it found it.
int arg_is_whole_record (void);

// Call named.
void call_named (int is_fn, const char *name, value_t *retval);

// PROC / FN call written with the keyword prefix (PROCname / FNname). The current
// token is KW_PROC or KW_FN with the name in tok_var; consume it, then delegate.
void call_proc (int is_fn, value_t *retval);

#endif /* INTERP_CALL_H */
