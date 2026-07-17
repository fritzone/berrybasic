// ===========================================================================
// BerryBasiC — keyword table and lexer
//
// This file is a fragment of the interpreter and is #included by basic.c.
// It is NOT a standalone translation unit: it shares the single translation
// unit's static state and is compiled only as part of basic.c. Do not add it
// to the build system or compile it on its own.
// ===========================================================================
// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

enum {
    T_EOL, T_NUM, T_STR, T_VAR, T_KW, T_LABEL,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_CARET,
    T_LP, T_RP, T_COMMA, T_SEMI, T_COLON, T_SQUOTE,
    T_EQ, T_NE, T_LT, T_GT, T_LE, T_GE,
    T_QUERY, T_PLING, T_DOLLAR,         // ? ! $ memory indirection
    T_HASH                              // # file channel prefix
};

enum {
    // Keywords
    KW_PRINT, KW_LET, KW_GOTO, KW_IF, KW_THEN,
    KW_REM, KW_END, KW_RUN, KW_LIST, KW_NEW,
    KW_FOR, KW_TO, KW_STEP, KW_NEXT, KW_GOSUB, KW_RETURN, KW_INPUT, KW_DIM, KW_PI,
    KW_REPEAT, KW_UNTIL, KW_ELSE, KW_CLS, KW_COLOUR,
    KW_WHILE, KW_ENDWHILE, KW_ENDIF,                  // structured loops / block IF
    KW_CASE, KW_OF, KW_WHEN, KW_OTHERWISE, KW_ENDCASE, // CASE selection
    KW_EXIT, KW_CONTINUE,                              // loop control (break / continue)
    KW_TRY, KW_CATCH, KW_ENDTRY, KW_RAISE,             // structured error handling
    KW_DEF, KW_PROC, KW_FN, KW_ENDPROC, KW_LOCAL,     // procedures & functions
    KW_TYPE, KW_ENDTYPE, KW_AS,                       // user-defined types (records)
    KW_IMPORT,                                        // IMPORT "module": pull in its PROC/FN
    KW_DIV, KW_MOD, KW_AND, KW_OR, KW_EOR, KW_NOT,    // operator keywords
    KW_ON, KW_DATA, KW_READ, KW_RESTORE, KW_STOP, KW_VDU, KW_TIME,  // statements
    KW_WAIT,                                                        // WAIT: pace to ~60 Hz
    KW_DELAY,                                                       // DELAY cs: pause for n centiseconds
    KW_POKE,                                                        // POKE addr,byte (alias for ?addr=byte)
    KW_EXEC,                                                        // EXEC "stmt" : run a string as BASIC
    KW_PINMODE, KW_PINSET, KW_PINCLR,                              // GPIO statements
    KW_OUTPUT, KW_PULLUP, KW_PULLDOWN, KW_ALT,                    // PINMODE sub-keywords (INPUT reused)
    KW_I2CWRITE, KW_I2CREAD,                                       // I2C statements
    KW_SOUND, KW_TONE,                                             // audio statements
    KW_MODE, KW_GCOL, KW_PLOT, KW_MOVE, KW_DRAW, KW_CLG,            // graphics statements
    KW_LINE, KW_RECTANGLE, KW_CIRCLE, KW_ELLIPSE, KW_FILL,         // shape commands
    KW_GGET, KW_GPUT, KW_SAVESPRITE,                              // sprite capture / stamp / save
    KW_BUFFER, KW_FLIP,                                           // double buffering: BUFFER ON/OFF + FLIP
    KW_GTINT,                                                     // GTINT r,g,b,a / GTINT OFF : sprite tint
    KW_NEWSPRITE, KW_SPRITETARGET,                               // render-to-sprite: blank sprite + redirect drawing
    KW_TILEMAP,                                                  // TILEMAP sheet,map,cols,rows,tw,th,sx,sy
    KW_FONT, KW_FONTSIZE, KW_FONTSTYLE, KW_GTEXT,                // TrueType fonts: select / size / style / draw
    KW_SAVE, KW_LOAD, KW_CAT, KW_DIR, KW_DELETE,                    // storage statements
    KW_MKDIR, KW_CD, KW_RMDIR, KW_PWD,                              // directory statements
    KW_BPUT, KW_CLOSE,                                              // file I/O statements
    KW_SEED,                                                        // load a native seed
    KW_AUTO, KW_RENUMBER, KW_EDIT,                                  // editor commands
    KW_MOUSE,                                                       // MOUSE x,y,b statement
    KW_TRUE, KW_FALSE, KW_POS, KW_VPOS,               // parenless value keywords
    KW_PINS,                                          // PINS : read all header pins at once
    KW_SCREEN, KW_SCREENW, KW_SCREENH,                // SCREEN statement + current-resolution values
    KW_FONTHEIGHT,                                    // FONTHEIGHT : line height of the current font (px)
    KW_KEYBOARD, KW_KEYBOARDS,                        // KEYBOARD "NO" statement + KEYBOARD$ (current layout)
    KW_KEYMOD,                                        // KEYMOD : modifier/lock bitmask
    KW_ERR, KW_ERRS,                                  // ERR / ERR$ : code + message of a caught error
    KW_MOUSEX, KW_MOUSEY, KW_MOUSEB,                  // mouse pointer x / y / buttons
    KW_DIRNEXT, KW_DIRSIZE, KW_DIRTYPE,               // directory-scan cursor fields
    KW_DIRNAMES, KW_DIRDATES, KW_DIRTIMES,            // (DIRNAME$/DIRDATE$/DIRTIME$)
    KW_NEWDICT, KW_NEWLIST, KW_NEWTREE,               // create a new dictionary / list / tree
    KW_DICTSET, KW_DICTDEL,                           // dictionary statements
    KW_PUSH, KW_LISTSET, KW_LISTINS, KW_LISTDEL,      // list statements
    KW_TREESET, KW_TREEDEL,                           // tree statements
    // Functions from the "standard library"
    KW_ABS, KW_INT, KW_SGN, KW_SQR, KW_SIN, KW_COS, KW_TAN, KW_ATN,
    KW_LOG, KW_EXP, KW_DEG, KW_RAD, KW_ACS, KW_ASN,
    KW_RND, KW_LEN, KW_ASC, KW_VAL, KW_INSTR, KW_GET, KW_INKEY,
    KW_CHRS, KW_STRS, KW_LEFTS, KW_RIGHTS, KW_MIDS, KW_STRINGS, KW_GETS, KW_INKEYS,
    KW_POINT,                                         // POINT(x,y) graphics function
    KW_SHL, KW_SHR, KW_ASR, KW_ROL, KW_ROR,           // bitwise shift / rotate functions
    KW_CALL, KW_CALLS,                                // CALL()/CALL$(): invoke a native seed
    KW_OPENIN, KW_OPENOUT, KW_OPENUP,                 // open a file -> channel
    KW_BGET, KW_EOF, KW_EXT, KW_PTR,                  // BGET#/EOF#/EXT#/PTR# channel functions
    KW_RGB,                                           // RGB(r,g,b) -> packed truecolour value
    KW_LOADSPRITE,                                    // LOADSPRITE("file") -> sprite address
    KW_SPRW, KW_SPRH,                                 // SPRW(addr)/SPRH(addr): sprite size
    KW_UPPERS, KW_LOWERS, KW_TRIMS, KW_REPLACES,      // string library: case, trim, replace
    KW_CONTAINS, KW_STARTSWITH, KW_ENDSWITH,          // string predicates -> TRUE/FALSE
    KW_SPLIT, KW_JOINS,                               // SPLIT(...) -> array ; JOIN$(array,...)
    KW_PEEK,                                          // PEEK(addr) -> byte (alias for ?addr)
    KW_PIN,                                           // PIN(n) -> level (also a statement: PIN n,v)
    KW_PINWAIT,                                       // PINWAIT(pin,edge,timeout) -> pin or -1
    KW_EVAL,                                          // EVAL("expr") -> value of the parsed expression
    KW_DIROPEN,                                       // DIROPEN("path") -> start a scan
    // Collection accessor functions (see the NEW* creators above)
    KW_SIZE,                                          // SIZE(h) : element count of any collection
    KW_DICTGET, KW_DICTGETS, KW_DICTHAS, KW_DICTKEYS, // dictionary lookups + key iteration
    KW_POP, KW_POPS, KW_LISTGET, KW_LISTGETS,         // list read/remove
    KW_TREEGET, KW_TREEGETS, KW_TREEHAS,              // tree lookups
    KW_TREEMIN, KW_TREEMAX, KW_TREEKEY,               // tree ordered access
    KW_I2CPROBE,                                      // I2CPROBE(addr) -> device present?
    KW_HEXS, KW_BINS, KW_FORMATS,                     // formatted / multi-base output
    KW_LOADFONT, KW_TEXTWIDTH,                         // LOADFONT("f")->handle ; TEXTWIDTH(s$)->pixels
    KW__FIRST_FUNC = KW_ABS, KW__LAST_FUNC = KW_TEXTWIDTH,
    KW_TAB, KW_SPC, KW_USING                          // PRINT-only modifiers
};

static int is_func_kw(int id) { return id >= KW__FIRST_FUNC && id <= KW__LAST_FUNC; }

// Keywords registered by native seeds (see SEED_KEYWORD) get token ids from this
// base upward, well clear of the built-in KW_* enum. The lexer recognises their
// names via seed_kw_lookup (defined with the seed machinery, forward-declared
// here); is_seed_kw() tells the two apart during dispatch.
#define KW_SEED_DYN 5000
static int seed_kw_lookup(const char *name);   // -> registry index, or -1
static const char *seed_kw_name(int idx);      // registry index -> name, or 0
static int is_seed_kw(int id) { return id >= KW_SEED_DYN; }

static const struct { const char *name; int id; } kwtab[] = {
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
    { "MKDIR", KW_MKDIR }, { "CD", KW_CD }, { "RMDIR", KW_RMDIR }, { "PWD", KW_PWD },
    { "AUTO",  KW_AUTO  }, { "RENUMBER", KW_RENUMBER }, { "EDIT", KW_EDIT },
    { "TIME",  KW_TIME  }, { "TRUE", KW_TRUE  }, { "FALSE",  KW_FALSE  },
    { "POS",   KW_POS   }, { "VPOS", KW_VPOS  },
    { "SCREEN", KW_SCREEN }, { "SCREENW", KW_SCREENW }, { "SCREENH", KW_SCREENH },
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
    { "EVAL", KW_EVAL }, { "EXEC", KW_EXEC },
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
static const int kwcount = (int)(sizeof(kwtab) / sizeof(kwtab[0]));

// The canonical spelling of a keyword id, or 0 if there isn't one. Used where a
// keyword's *word* is wanted rather than its meaning - a type or field name is
// allowed to be a word like POINT or SIZE, because it can only appear where
// nothing else is legal. Aliases (COLOUR/COLOR) resolve to whichever is listed
// first, which is all a name needs.
static const char *kw_spelling(int id) {
    for (int i = 0; i < kwcount; i++)
        if (kwtab[i].id == id) return kwtab[i].name;
    return 0;
}

static const char *cur_text;           // text of the line currently executing (see cur_line_idx)
static const char *lx;                 // lexer cursor into the current line
static const char *tok_start;          // start of the current token (for re-branching)
static int    tok;                     // current token type
static double tok_num;                 // payload for T_NUM
static int  tok_kw;                    // payload for T_KW
static char tok_str[LINE_LEN];         // payload for T_STR
static char tok_var[NAME_LEN];         // payload for T_VAR

static void lex_next(void) {
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
typedef struct {
    const char *cur_text, *lx, *tok_start;
    int    tok, tok_kw;
    double tok_num;
    char   tok_str[LINE_LEN];
    char   tok_var[NAME_LEN];
} lexstate_t;

static void lex_save(lexstate_t *s) {
    s->cur_text = cur_text; s->lx = lx; s->tok_start = tok_start;
    s->tok = tok; s->tok_kw = tok_kw; s->tok_num = tok_num;
    s_copy(s->tok_str, tok_str, LINE_LEN);
    s_copy(s->tok_var, tok_var, NAME_LEN);
}
static void lex_restore(const lexstate_t *s) {
    cur_text = s->cur_text; lx = s->lx; tok_start = s->tok_start;
    tok = s->tok; tok_kw = s->tok_kw; tok_num = s->tok_num;
    s_copy(tok_str, s->tok_str, LINE_LEN);
    s_copy(tok_var, s->tok_var, NAME_LEN);
}

