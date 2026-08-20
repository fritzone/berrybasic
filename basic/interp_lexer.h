#ifndef INTERP_LEXER_H
#define INTERP_LEXER_H

#include "interp_types.h"

/* ==================================================================
 * interp_lexer.c -- The keyword table and the on-the-fly tokeniser (lexer).
 * ================================================================== */

/* -------------------------------------------------------------- globals */

extern const char *cur_text;   // text of the line currently executing (see cur_line_idx)
extern const int kwcount;
extern const kwent_t kwtab[];
extern const char *lx;   // lexer cursor into the current line
extern int    tok;   // current token type
extern int  tok_kw;   // payload for T_KW
extern double tok_num;   // payload for T_NUM
extern const char *tok_start;   // start of the current token (for re-branching)
extern char tok_str[LINE_LEN];   // payload for T_STR
extern char tok_var[NAME_LEN];   // payload for T_VAR

/* ------------------------------------------------------------ functions */

// Predicate: non-zero when func kw.
int is_func_kw (int id);

// Predicate: non-zero when seed kw.
int is_seed_kw (int id);

// The canonical spelling of a keyword id, or 0 if there isn't one. Used where a
// keyword's *word* is wanted rather than its meaning - a type or field name is
// allowed to be a word like POINT or SIZE, because it can only appear where
// nothing else is legal. Aliases (COLOUR/COLOR) resolve to whichever is listed
// first, which is all a name needs.
const char *kw_spelling (int id);
int name_is_reserved (const char *name);

// Lex next.
void lex_next (void);

// Lex save.
void lex_save (lexstate_t *s);

// Lex restore.
void lex_restore (const lexstate_t *s);

#endif /* INTERP_LEXER_H */
