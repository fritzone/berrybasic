#ifndef INTERP_EVAL_H
#define INTERP_EVAL_H

#include "interp_types.h"

/* ==================================================================
 * interp_eval.c -- The recursive-descent expression evaluator.
 * ================================================================== */

/* ------------------------------------------------------------ functions */

// Byte-equality of two n-byte spans (used by the string search/replace helpers).
int mem_eq (const char *a, const char *b, int n);

// Parse an array reference written with empty parentheses, NAME() , as used by
// SPLIT and JOIN$. Fills *name and leaves the caller to find/create the array.
int parse_array_ref (char *name);

// Render a number as text under a FORMAT$/PRINT USING template. Returns the
// output length, or -1 if the template has no digit position at all. Template
// chars: '#' = optional digit (blank-filled), '0' = zero-filled digit, '.' =
// decimal point (positions after it set the decimal places, value is rounded),
// ',' = thousands separators in the integer part, a leading '+' forces a sign,
// a leading '-' is the default (sign only when negative). Any other character
// is literal and copied through. The integer part is never truncated: if it
// needs more digits than the template provides, the field simply grows.
int fmt_number (char *out, const char *tmpl, int tlen, double v);

// Expression evaluator: evaluate function.
value_t eval_function (int fn);

// Read a CR-terminated string from memory (the BBC $ indirection).
value_t mem_read_str (long int a);

// Mem write str.
void mem_write_str (long int a, const char *s, int len);

// Prim base.
value_t prim_base (void);

// A primary, plus the binary indirection postfix: base?offset (byte at base+off)
// and base!offset (word at base+off). Binds tighter than the arithmetic ops, so
// P%?I + 1 is (P%?I) + 1, and the offset is itself a primary.
value_t eval_primary (void);

// Exponentiation, tighter than unary minus (so -2^2 = -4), left-associative.
// The exponent is a sign-prefixed primary, so 2^-3 and 2^3^2 (=(2^3)^2) work.
value_t eval_power (void);

// Expression evaluator: evaluate unary.
value_t eval_unary (void);

// Expression evaluator: evaluate term.
value_t eval_term (void);

// Expression evaluator: evaluate add.
value_t eval_add (void);

// Str cmp.
int str_cmp (value_t a, value_t b);

// Relational comparison. BBC BASIC TRUE is -1 (all bits set), FALSE is 0.
value_t eval_compare (void);

// Expression evaluator: evaluate and.
value_t eval_and (void);

// Expression evaluator: evaluate expr.
value_t eval_expr (void);

#endif /* INTERP_EVAL_H */
