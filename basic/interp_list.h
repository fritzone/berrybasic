#ifndef INTERP_LIST_H
#define INTERP_LIST_H

#include "interp_types.h"

/* ==================================================================
 * interp_list.c -- LIST - both the plain and the pretty (coloured, indented) forms.
 * ================================================================== */

/* ------------------------------------------------------------ functions */

// List text.
void list_text (const char *t);

// Predicate: non-zero when hexd.
int is_hexd (char c);

// Num digits.
int num_digits (int n);

// Analyse one program line for block indentation. Returns the net change in
// nesting depth the line makes (openers +1, closers -1), and sets *dedent_first
// when the line *begins* with a closer or a mid-block keyword (NEXT, ENDIF,
// ELSE, ...) so the line itself renders one level out. A multi-line IF is one
// whose last word is THEN; a one-line DEF (a classic FN= or a PROC:...:ENDPROC)
// nets to zero, so only true multi-line definitions open a block.
int line_blocks (const char *t, int *dedent_first);

// Render a program line with syntax colouring: keywords upper-cased and coloured,
// strings, numbers, REM comments and PROC/FN names each in their own colour, the
// rest (variables, operators) in the default. Mirrors list_text's word handling.
void list_fancy_text (const char *t);

// LIST [SIMPLE] [start][,end] : whole program, a single line (LIST 100), a range
// (LIST 100,200), from a line (LIST 100,) or up to a line (LIST ,200). By default
// the listing is pretty-printed - line numbers right-aligned in a gutter sized to
// the largest number, structural indentation, and syntax colouring. LIST SIMPLE
// gives the plain "number space text" form (e.g. for copying or a mono terminal).
// LIST [SIMPLE] "file" lists a program file on the card instead of the one in
// memory (same pretty/plain forms), with screen paging.
void stmt_list (void);

#endif /* INTERP_LIST_H */
