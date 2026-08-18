#ifndef INTERP_UTIL_H
#define INTERP_UTIL_H

#include "interp_types.h"

/* ==================================================================
 * interp_util.c -- Small helpers: string/char utilities, error reporting, the hand-rolled
 * floating-point math library, and number formatting.
 * ================================================================== */

/* -------------------------------------------------------------- globals */

extern int g_err;   // set by err(); unwinds the current operation
extern int  g_err_reported;   // has the pending error already been printed?
extern int  g_errcode;   // numeric code (for ERR): 0 = none / built-in
extern int  g_errline;   // line the error happened on (for deferred report)
extern char g_errmsg[ERRMSG_MAX];   // message of the most recent error (for ERR$)
extern int g_runline;   // line number currently executing, or -1 immediate
extern int  try_open;   // lexically open TRY blocks (awaiting ENDTRY)
extern int  try_sp;   // depth of active TRY handlers (0 = none)

/* ------------------------------------------------------------ functions */

// Predicate: non-zero when space.
int is_space (char c);

// Predicate: non-zero when digit.
int is_digit (char c);

// Predicate: non-zero when alpha.
int is_alpha (char c);

// Predicate: non-zero when alnum.
int is_alnum (char c);

// Up.
char up (char c);

// S copy.
void s_copy (char *d, const char *s, int max);

// S eq.
int s_eq (const char *a, const char *b);

// True if the first n characters of a equal b (b must be at least n chars).
int s_eqn (const char *a, const char *b, int n);

// Set errmsg.
void set_errmsg (const char *msg);

// Print the " (line N)" suffix and the trailing newline of an error report.
void err_tail (void);

// Report an error and unwind the current operation; only the first one sticks.
// e.g. "Syntax error (line 30)".
void err (const char *msg);

// Two-part error, for a fixed prefix plus a dynamic name, e.g.
// err2("Expected ", "')'") -> "Expected ')' (line 30)".
void err2 (const char *a, const char *b);

// Con putn.
void con_putn (long int v);

// Con putsn.
void con_putsn (const char *s, int len);

// Dfloor.
double dfloor (double x);

// Dround.
double dround (double x);

// Dsqrt.
double dsqrt (double x);

// Dexp.
double dexp (double x);

// Dlog.
double dlog (double x);

// Dsin.
double dsin (double x);

// Dcos.
double dcos (double x);

// Dtan.
double dtan (double x);

// Datan.
double datan (double x);

// Dpow.
double dpow (double a, double b);

// Format v into out[] (needs ~32 bytes) as BASIC would, returning the length.
// Whole numbers print without a decimal point; otherwise up to 9 significant
// digits with trailing zeros trimmed; very large/small values use E notation.
int dbl_to_str (char *out, double v);

// Parse a number from s[0..len). *consumed receives how many chars were used.
double parse_double (const char *s, int len, int *consumed);

#endif /* INTERP_UTIL_H */
