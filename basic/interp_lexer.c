#include "interp_lexer.h"
#include "interp_util.h"
#include "interp_data.h"
#include "interp_seed.h"
#include "interp_stmt.h"
#include "interp_events.h"
#include "interp_pod.h"
// ===========================================================================
// BerryBasiC — keyword table and lexer
//
// A separately-compiled module of the interpreter. Its cross-module
// interface is declared in interp_lexer.h (extern globals + documented function
// prototypes) and interp_types.h (the shared data types).
// ===========================================================================
// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------



int is_func_kw(int id) { return id >= KW__FIRST_FUNC && id <= KW__LAST_FUNC; }

// Keywords registered by native seeds (see SEED_KEYWORD) get token ids from this
// base upward, well clear of the built-in KW_* enum. The lexer recognises their
// names via seed_kw_lookup (defined with the seed machinery, forward-declared
// here); is_seed_kw() tells the two apart during dispatch.
int seed_kw_lookup(const char *name);   // -> registry index, or -1
const char *seed_kw_name(int idx);      // registry index -> name, or 0
int is_seed_kw(int id) { return id >= KW_SEED_DYN; }

const kwent_t kwtab[] = {
    { "PRINT", KW_PRINT }, { "LET",  KW_LET   }, { "GOTO",   KW_GOTO   },
    { "IF",    KW_IF    }, { "THEN", KW_THEN  }, { "REM",    KW_REM    },
    { "END",   KW_END   }, { "RUN",  KW_RUN   }, { "LIST",   KW_LIST   },
    { "NEW",   KW_NEW   }, { "FOR",  KW_FOR   }, { "TO",     KW_TO     },
    { "STEP",  KW_STEP  }, { "NEXT", KW_NEXT  }, { "GOSUB",  KW_GOSUB  },
    { "RETURN",KW_RETURN }, { "INPUT", KW_INPUT }, { "DIM", KW_DIM },
    { "REPEAT",KW_REPEAT }, { "UNTIL", KW_UNTIL }, { "ELSE", KW_ELSE },
    { "WHILE", KW_WHILE }, { "ENDWHILE", KW_ENDWHILE }, { "ENDIF", KW_ENDIF },
    { "CASE",  KW_CASE  }, { "OF", KW_OF }, { "WHEN", KW_WHEN },
    { "OTHERWISE", KW_OTHERWISE }, { "ENDCASE", KW_ENDCASE },
    { "EXIT", KW_EXIT }, { "CONTINUE", KW_CONTINUE },
    { "TRY", KW_TRY }, { "CATCH", KW_CATCH }, { "ENDTRY", KW_ENDTRY }, { "RAISE", KW_RAISE },
    { "ERR", KW_ERR }, { "ERR$", KW_ERRS },
    { "CLS",   KW_CLS   }, { "COLOUR", KW_COLOUR }, { "COLOR", KW_COLOUR },
    { "DEF",   KW_DEF   }, { "ENDPROC", KW_ENDPROC }, { "LOCAL", KW_LOCAL },
    { "TYPE",  KW_TYPE  }, { "ENDTYPE", KW_ENDTYPE }, { "AS", KW_AS },
    { "IMPORT", KW_IMPORT },
    { "DIV",   KW_DIV   }, { "MOD",  KW_MOD   }, { "AND",    KW_AND    },
    { "OR",    KW_OR    }, { "EOR",  KW_EOR   }, { "NOT",    KW_NOT    },
    { "ON",    KW_ON    }, { "DATA", KW_DATA  }, { "READ",   KW_READ   },
    { "WAIT",  KW_WAIT  }, { "DELAY", KW_DELAY },
    { "RESTORE", KW_RESTORE }, { "STOP", KW_STOP }, { "VDU",  KW_VDU    },
    { "SOUND", KW_SOUND }, { "TONE", KW_TONE  },
    { "MODE",  KW_MODE  }, { "GCOL", KW_GCOL  }, { "PLOT",   KW_PLOT   },
    { "MOVE",  KW_MOVE  }, { "DRAW", KW_DRAW  }, { "CLG",    KW_CLG    },
    { "POINT", KW_POINT },
    { "LINE",  KW_LINE  }, { "RECTANGLE", KW_RECTANGLE }, { "CIRCLE", KW_CIRCLE },
    { "ELLIPSE", KW_ELLIPSE }, { "FILL", KW_FILL }, { "RGB", KW_RGB },
    { "LINEWIDTH", KW_LINEWIDTH }, { "LINEJOIN", KW_LINEJOIN }, { "LINECAP", KW_LINECAP },
    { "LOADSPRITE", KW_LOADSPRITE }, { "SPRW", KW_SPRW }, { "SPRH", KW_SPRH },
    { "DIROPEN", KW_DIROPEN }, { "DIRNEXT", KW_DIRNEXT },
    { "DIRNAME$", KW_DIRNAMES }, { "DIRSIZE", KW_DIRSIZE }, { "DIRTYPE", KW_DIRTYPE },
    { "DIRDATE$", KW_DIRDATES }, { "DIRTIME$", KW_DIRTIMES },
    { "GGET",  KW_GGET  }, { "GPUT", KW_GPUT }, { "SAVESPRITE", KW_SAVESPRITE },
    { "BUFFER", KW_BUFFER }, { "FLIP", KW_FLIP }, { "GTINT", KW_GTINT },
    { "NEWSPRITE", KW_NEWSPRITE }, { "SPRITETARGET", KW_SPRITETARGET },
    { "TILEMAP", KW_TILEMAP },
    { "LOADFONT", KW_LOADFONT }, { "FONT", KW_FONT }, { "FONTSIZE", KW_FONTSIZE },
    { "FONTSTYLE", KW_FONTSTYLE }, { "GTEXT", KW_GTEXT },
    { "TEXTWIDTH", KW_TEXTWIDTH }, { "FONTHEIGHT", KW_FONTHEIGHT },
    // Collections: dictionary, list, binary tree
    { "NEWDICT", KW_NEWDICT }, { "NEWLIST", KW_NEWLIST }, { "NEWTREE", KW_NEWTREE },
    { "SIZE", KW_SIZE },
    { "DICTSET", KW_DICTSET }, { "DICTDEL", KW_DICTDEL }, { "DICTHAS", KW_DICTHAS },
    { "DICTGET", KW_DICTGET }, { "DICTGET$", KW_DICTGETS }, { "DICTKEY$", KW_DICTKEYS },
    { "PUSH", KW_PUSH }, { "POP", KW_POP }, { "POP$", KW_POPS },
    { "LISTSET", KW_LISTSET }, { "LISTINS", KW_LISTINS }, { "LISTDEL", KW_LISTDEL },
    { "LISTGET", KW_LISTGET }, { "LISTGET$", KW_LISTGETS },
    { "TREESET", KW_TREESET }, { "TREEDEL", KW_TREEDEL }, { "TREEHAS", KW_TREEHAS },
    { "TREEGET", KW_TREEGET }, { "TREEGET$", KW_TREEGETS },
    { "TREEMIN", KW_TREEMIN }, { "TREEMAX", KW_TREEMAX }, { "TREEKEY", KW_TREEKEY },
    { "SAVE",  KW_SAVE  }, { "LOAD", KW_LOAD  }, { "CAT",    KW_CAT    },
    { "DIR",   KW_DIR   }, { "DELETE", KW_DELETE }, { "SEED", KW_SEED },
    { "PODLOAD", KW_PODLOAD }, { "PODFREE", KW_PODFREE },
    { "PODINFO", KW_PODINFO }, { "PODCAPS", KW_PODCAPS },
    { "MKDIR", KW_MKDIR }, { "CD", KW_CD }, { "RMDIR", KW_RMDIR }, { "PWD", KW_PWD },
    { "AUTO",  KW_AUTO  }, { "RENUMBER", KW_RENUMBER }, { "EDIT", KW_EDIT },
    { "TIME",  KW_TIME  }, { "TRUE", KW_TRUE  }, { "FALSE",  KW_FALSE  },
    { "POS",   KW_POS   }, { "VPOS", KW_VPOS  },
    { "SCREEN", KW_SCREEN }, { "SCREENW", KW_SCREENW }, { "SCREENH", KW_SCREENH },
    { "GMODE", KW_GMODE },
    { "KEYBOARD", KW_KEYBOARD }, { "KEYBOARD$", KW_KEYBOARDS },
    { "KEYMOD", KW_KEYMOD },
    { "MOUSE", KW_MOUSE }, { "MOUSEX", KW_MOUSEX },
    { "MOUSEY", KW_MOUSEY }, { "MOUSEB", KW_MOUSEB },
    { "PI",    KW_PI    },
    { "ABS",   KW_ABS   }, { "INT",  KW_INT   }, { "SGN",    KW_SGN    },
    { "SQR",   KW_SQR   }, { "SIN",  KW_SIN   }, { "COS",    KW_COS    },
    { "TAN",   KW_TAN   }, { "ATN",  KW_ATN   }, { "LOG",    KW_LOG    },
    { "EXP",   KW_EXP   }, { "DEG",  KW_DEG   }, { "RAD",    KW_RAD    },
    { "ACS",   KW_ACS   }, { "ASN",  KW_ASN   },
    { "INSTR", KW_INSTR }, { "GET",  KW_GET   }, { "INKEY",  KW_INKEY  },
    { "RND",   KW_RND   }, { "LEN",  KW_LEN   }, { "ASC",    KW_ASC    },
    { "VAL",   KW_VAL   }, { "CHR$", KW_CHRS  }, { "STR$",   KW_STRS   },
    { "LEFT$", KW_LEFTS }, { "RIGHT$", KW_RIGHTS }, { "MID$",  KW_MIDS  },
    { "STRING$", KW_STRINGS }, { "GET$", KW_GETS }, { "INKEY$", KW_INKEYS },
    { "UPPER$", KW_UPPERS }, { "LOWER$", KW_LOWERS }, { "TRIM$", KW_TRIMS },
    { "REPLACE$", KW_REPLACES }, { "CONTAINS", KW_CONTAINS },
    { "STARTSWITH", KW_STARTSWITH }, { "ENDSWITH", KW_ENDSWITH },
    { "SPLIT", KW_SPLIT }, { "JOIN$", KW_JOINS },
    { "PEEK", KW_PEEK }, { "POKE", KW_POKE },
    { "EVAL", KW_EVAL }, { "EXEC", KW_EXEC }, { "CHAIN", KW_CHAIN },
    { "PINMODE", KW_PINMODE }, { "PIN", KW_PIN }, { "PINS", KW_PINS },
    { "PINSET", KW_PINSET }, { "PINCLR", KW_PINCLR }, { "PINWAIT", KW_PINWAIT },
    { "I2CWRITE", KW_I2CWRITE }, { "I2CREAD", KW_I2CREAD }, { "I2CPROBE", KW_I2CPROBE },
    { "OUTPUT", KW_OUTPUT }, { "PULLUP", KW_PULLUP },
    { "PULLDOWN", KW_PULLDOWN }, { "ALT", KW_ALT },
    { "SHL",   KW_SHL   }, { "SHR",  KW_SHR   }, { "ASR",    KW_ASR    },
    { "ROL",   KW_ROL   }, { "ROR",  KW_ROR   },
    { "CALL",  KW_CALL  }, { "CALL$", KW_CALLS },
    { "OPENIN", KW_OPENIN }, { "OPENOUT", KW_OPENOUT }, { "OPENUP", KW_OPENUP },
    { "BGET",  KW_BGET  }, { "BPUT", KW_BPUT }, { "EOF", KW_EOF },
    { "EXT",   KW_EXT   }, { "PTR",  KW_PTR  }, { "CLOSE", KW_CLOSE },
    { "TAB",   KW_TAB   }, { "SPC",  KW_SPC   }, { "USING", KW_USING },
    { "HEX$",  KW_HEXS  }, { "BIN$", KW_BINS  }, { "FORMAT$", KW_FORMATS },
};
const int kwcount = (int)(sizeof(kwtab) / sizeof(kwtab[0]));

// The canonical spelling of a keyword id, or 0 if there isn't one. Used where a
// keyword's *word* is wanted rather than its meaning - a type or field name is
// allowed to be a word like POINT or SIZE, because it can only appear where
// nothing else is legal. Aliases (COLOUR/COLOR) resolve to whichever is listed
// first, which is all a name needs.
const char *kw_spelling(int id) {
    for (int i = 0; i < kwcount; i++)
        if (kwtab[i].id == id) return kwtab[i].name;
    return 0;
}

const char *cur_text;           // text of the line currently executing (see cur_line_idx)
const char *lx;                 // lexer cursor into the current line
const char *tok_start;          // start of the current token (for re-branching)
int    tok;                     // current token type
double tok_num;                 // payload for T_NUM
int  tok_kw;                    // payload for T_KW
char tok_str[LINE_LEN];         // payload for T_STR
char tok_var[NAME_LEN];         // payload for T_VAR

void lex_next(void) {
    while (is_space(*lx)) lx++;
    tok_start = lx;
    char c = *lx;

    if (c == 0) { tok = T_EOL; return; }

    // ".name" is a branch label (definition at a line's start, or a GOTO/GOSUB
    // target) and, after a record variable, a field selector: p.x lexes as the
    // variable P followed by the label X, which is why field access needs no
    // token of its own. The $/% suffix is for fields (p.name$); a label never
    // has one, so accepting it here costs labels nothing.
    // A leading '.' followed by a digit is still a number (handled below).
    if (c == '.' && is_alpha(lx[1])) {
        lx++;
        char id[NAME_LEN]; int n = 0;
        while (is_alnum(*lx)) { if (n < NAME_LEN - 1) id[n++] = up(*lx); lx++; }
        if ((*lx == '$' || *lx == '%') && n < NAME_LEN - 1) { id[n++] = *lx; lx++; }
        id[n] = 0;
        s_copy(tok_var, id, NAME_LEN);
        tok = T_LABEL; return;
    }

    if (is_digit(c) || (c == '.' && is_digit(lx[1]))) {
        double val = 0;
        while (is_digit(*lx)) { val = val * 10 + (*lx - '0'); lx++; }
        if (*lx == '.') {
            lx++;
            double f = 0, sc = 1;
            while (is_digit(*lx)) { f = f * 10 + (*lx - '0'); sc *= 10; lx++; }
            val += f / sc;
        }
        if (*lx == 'E' || *lx == 'e') {          // exponent (only if real digits follow)
            const char *save = lx;
            lx++;
            int es = 1;
            if (*lx == '+' || *lx == '-') { if (*lx == '-') es = -1; lx++; }
            if (is_digit(*lx)) {
                int ev = 0;
                while (is_digit(*lx)) { ev = ev * 10 + (*lx - '0'); lx++; }
                double p = 1; for (int k = 0; k < ev; k++) p *= 10;
                val = es < 0 ? val / p : val * p;
            } else {
                lx = save;
            }
        }
        tok = T_NUM; tok_num = val; return;
    }

    if (c == '&') {                          // BBC hexadecimal constant, e.g. &FF
        lx++;
        long h = 0;
        while (1) {
            char d = up(*lx);
            if (d >= '0' && d <= '9') h = h * 16 + (d - '0');
            else if (d >= 'A' && d <= 'F') h = h * 16 + (d - 'A' + 10);
            else break;
            lx++;
        }
        tok = T_NUM; tok_num = (double)h; return;
    }

    if (c == '%' && (lx[1] == '0' || lx[1] == '1')) {  // binary constant, e.g. %1010
        lx++;                                          // (a lone '%' is a var suffix,
        long b = 0;                                    //  handled in the identifier branch)
        while (*lx == '0' || *lx == '1') { b = b * 2 + (*lx - '0'); lx++; }
        tok = T_NUM; tok_num = (double)b; return;
    }

    if (c == '"') {
        lx++;
        int n = 0;
        while (*lx && *lx != '"') { if (n < LINE_LEN - 1) tok_str[n++] = *lx; lx++; }
        tok_str[n] = 0;
        if (*lx == '"') lx++;
        tok = T_STR; return;
    }

    if (is_alpha(c)) {
        const char *id_start = lx;                 // for keyword-prefix splitting
        char id[16];
        int  n = 0;
        while (is_alnum(*lx)) { if (n < 15) id[n++] = up(*lx); lx++; }
        if ((*lx == '$' || *lx == '%') && n < 15) { id[n++] = *lx; lx++; }  // $=string %=integer
        id[n] = 0;
        // BBC PROCname / FNname glue the name to the keyword; the spaced forms
        // (DEF fn NAME, END proc) leave tok_var empty. Either way, PROC/FN here
        // set tok_var so callers can tell glued from bare reliably.
        if (id[0]=='P'&&id[1]=='R'&&id[2]=='O'&&id[3]=='C') {
            tok = T_KW; tok_kw = KW_PROC; s_copy(tok_var, id + 4, NAME_LEN); return;
        }
        if (id[0]=='F'&&id[1]=='N') {
            tok = T_KW; tok_kw = KW_FN; s_copy(tok_var, id + 2, NAME_LEN); return;
        }
        for (int i = 0; i < kwcount; i++)
            if (s_eq(id, kwtab[i].name)) { tok = T_KW; tok_kw = kwtab[i].id; return; }
        // A keyword a seed added to the language (matched as a whole word, after
        // the built-ins so a seed can't shadow one).
        { int sk = seed_kw_lookup(id);
          if (sk >= 0) { tok = T_KW; tok_kw = KW_SEED_DYN + sk; return; } }
        // BBC tokenises a function keyword glued to a numeric argument: SQR3 = SQR 3.
        // If a function keyword is a prefix of this word and is immediately followed
        // by a digit, emit the keyword and re-read the rest from after it. Limiting
        // this to a following digit keeps word-like variable names (SINE, VALUE...).
        for (int i = 0; i < kwcount; i++) {
            if (!is_func_kw(kwtab[i].id)) continue;
            int L = 0; while (kwtab[i].name[L]) L++;
            if (L < n && is_digit(id[L]) && s_eqn(id, kwtab[i].name, L)) {
                lx = id_start + L;                 // re-lex the argument next time
                tok = T_KW; tok_kw = kwtab[i].id; return;
            }
        }
        s_copy(tok_var, id, NAME_LEN);
        tok = T_VAR; return;
    }

    lx++;                              // single/double-char operator
    switch (c) {
        case '+': tok = T_PLUS;  return;
        case '-': tok = T_MINUS; return;
        case '*': tok = T_STAR;  return;
        case '/': tok = T_SLASH; return;
        case '^': tok = T_CARET; return;
        case '(': tok = T_LP;    return;
        case ')': tok = T_RP;    return;
        case ',': tok = T_COMMA; return;
        case ';': tok = T_SEMI;  return;
        case '\'': tok = T_SQUOTE; return;   // PRINT ' -> newline
        case ':': tok = T_COLON; return;
        case '=': tok = T_EQ;    return;
        case '<':
            if (*lx == '=') { lx++; tok = T_LE; return; }
            if (*lx == '>') { lx++; tok = T_NE; return; }
            tok = T_LT; return;
        case '>':
            if (*lx == '=') { lx++; tok = T_GE; return; }
            tok = T_GT; return;
        case '?': tok = T_QUERY;  return;    // byte indirection
        case '!': tok = T_PLING;  return;    // 32-bit word indirection
        case '$': tok = T_DOLLAR; return;    // string indirection (a leading $)
        case '#': tok = T_HASH;   return;    // file channel prefix (BGET#, PTR#, ...)
        default: err("I don't recognise that character"); tok = T_EOL; return;
    }
}

// A snapshot of the whole lexer position, so EVAL / EXEC can point the lexer at
// a brand-new source string, run it, and put everything back exactly as it was.
// Everything the lexer/parser reads to decide "where am I" lives here.

void lex_save(lexstate_t *s) {
    s->cur_text = cur_text; s->lx = lx; s->tok_start = tok_start;
    s->tok = tok; s->tok_kw = tok_kw; s->tok_num = tok_num;
    s_copy(s->tok_str, tok_str, LINE_LEN);
    s_copy(s->tok_var, tok_var, NAME_LEN);
}
void lex_restore(const lexstate_t *s) {
    cur_text = s->cur_text; lx = s->lx; tok_start = s->tok_start;
    tok = s->tok; tok_kw = s->tok_kw; tok_num = s->tok_num;
    s_copy(tok_str, s->tok_str, LINE_LEN);
    s_copy(tok_var, s->tok_var, NAME_LEN);
}

