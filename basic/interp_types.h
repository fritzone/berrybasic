#ifndef INTERP_TYPES_H
#define INTERP_TYPES_H

#include "interp_base.h"

/* Every interpreter data type lives here. The modules pass these types
 * across their boundaries constantly, so keeping them in one header (rather
 * than one per module) avoids circular #includes. Each section below
 * corresponds to the module that owns/uses those types. */

/* ===================== interp_data.c ===================== */

// A program line. `module` is 0 for the main program and 1+ for lines pulled in
// by IMPORT; line-number lookups (GOTO/GOSUB/labels/DATA) are scoped to the
// running line's module, so a module's line numbers may freely overlap the main
// program's. Module lines are appended at RUN and stripped when it ends.
typedef struct { int num; int module; char text[LINE_LEN]; } progline_t;

typedef struct {
    int    is_str;
    double num;
    char  *str;
    int    len;
} value_t;

// A string value living in the GC heap: bytes pointer + length.
typedef struct { char *sptr; int slen; } strdesc_t;

typedef struct {
    char      name[NAME_LEN];
    int       is_str;
    int       is_int;        // % suffix: value is kept truncated to an integer
    double    num;
    strdesc_t s;             // string bytes live in the GC heap (gcheap)
    // Record variables (DIM p AS point). When is_rec is set, num/s are unused
    // and the fields below say where this record's storage lives; see the
    // user-defined type section for what the offsets index into.
    int       is_rec;
    int       rtype;         // index into types[]
    int       nelem;         // 1 = scalar record; >1 = array of records
    int       roff_num;      // base index into rec_nums[]
    int       roff_str;      // base index into rec_strs[]
} var_t;

typedef struct {
    char name[NAME_LEN];
    int  is_str;
    int  is_int;               // % suffix: integer elements
    int  ndim;
    int  dim[MAX_DIMS];        // element count per dimension (DIM value + 1)
    int  total;                // product of dims
    int  off;                  // base index into arr_nums[] or arr_strs[]
} arr_t;

typedef struct {
    char name[NAME_LEN];
    int  nf;                        // field count
    char fname[MAX_FIELDS][NAME_LEN];
    int  fis_str[MAX_FIELDS];
    int  fis_int[MAX_FIELDS];
    int  fslot[MAX_FIELDS];         // index among this element's numeric or string slots
    int  n_num, n_str;              // slots per element
} type_t;

typedef struct { var_t *slot; var_t old; } localsave_t;

/* ===================== interp_lexer.c ===================== */

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
    KW_CHAIN,                                                       // CHAIN c$[,r$] : run another .BAS, then optionally r$
    KW_PINMODE, KW_PINSET, KW_PINCLR,                              // GPIO statements
    KW_OUTPUT, KW_PULLUP, KW_PULLDOWN, KW_ALT,                    // PINMODE sub-keywords (INPUT reused)
    KW_I2CWRITE, KW_I2CREAD,                                       // I2C statements
    KW_SOUND, KW_TONE,                                             // audio statements
    KW_MODE, KW_GCOL, KW_PLOT, KW_MOVE, KW_DRAW, KW_CLG,            // graphics statements
    KW_LINE, KW_RECTANGLE, KW_CIRCLE, KW_ELLIPSE, KW_FILL,         // shape commands
    KW_LINEWIDTH, KW_LINEJOIN, KW_LINECAP,                        // thick-line pen
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
    KW_PODLOAD, KW_PODFREE, KW_PODINFO, KW_PODCAPS,                 // POD executables
    KW_AUTO, KW_RENUMBER, KW_EDIT,                                  // editor commands
    KW_MOUSE,                                                       // MOUSE x,y,b statement
    KW_TRUE, KW_FALSE, KW_POS, KW_VPOS,               // parenless value keywords
    KW_PINS,                                          // PINS : read all header pins at once
    KW_SCREEN, KW_SCREENW, KW_SCREENH,                // SCREEN statement + current-resolution values
    KW_GMODE,                                         // GMODE : active graphics mode (1 or 2)
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

/* One entry of the keyword name -> token-id table. */
typedef struct { const char *name; int id; } kwent_t;

/* ===================== interp_parse.c ===================== */

// Where a resolved field lives, and what kind it is.
typedef struct { int f; int slot; int is_str; int is_int; } fieldref_t;

// A resolved target: a scalar variable, an array element, or a record field.
// Exactly one of nump/strp is set, according to is_str.
typedef struct {
    int        is_str, is_int;
    double    *nump;
    strdesc_t *strp;
} target_t;

/* ===================== interp_seed.c ===================== */

enum { SEED_MAX_ARGS = 16 };

// A loaded SEED/CALL seed. There is no fixed number of them and no fixed size:
// each seed's code is a page-aligned block carved from the run heap and sized to
// the blob, and the record table grows on demand. Both are wiped on RUN/NEW (a
// program reloads its own seeds), so seed_recs is reset to 0 there. Handle =
// record index + 1.
typedef struct { int used; seed_entry entry; } seed_rec_t;

typedef union seed_hdr_u {
    struct { union seed_hdr_u *next; unsigned size; } s;  // size in header units
    long double _align;                                   // force 16-byte units
} seed_blk;

// A stored value: a number, or a string copied into the general heap.
typedef struct { int is_str; double num; char *str; int len; } cval_t;

typedef struct { cval_t *item; int len, cap; } list_t;

// --- DICT: an insertion-ordered array of (key, value) entries --------------
typedef struct { char *key; int klen; cval_t val; } dent_t;

typedef struct { dent_t *e; int len, cap; } dict_t;

typedef struct tnode { double key; cval_t val; struct tnode *l, *r; } tnode_t;

typedef struct { tnode_t *root; int count; } tree_t;

enum { CT_FREE = 0, CT_DICT, CT_LIST, CT_TREE };

// A dynamic (runtime-registered) keyword. Both native seeds (from /seed at
// startup) and POD extensions (PODLOAD) live in this one table, so the lexer and
// dispatch treat them alike. is_pod picks the backend: a native seed runs from
// its persistent code block via `entry`; a POD keyword runs from pod_entry
// (inside a resident POD image) through its own capability-gated pod_svc.
typedef struct {
    char name[16]; int kind; int minargs; int maxargs;
    int is_pod;              // 0 = native seed, 1 = POD keyword
    int slot;               // POD: pod_pool index (PODFREE drops its keywords). native: -1
    const void *entry;      // native seed: entry pointer into its persistent block
    const void *pod_entry, *pod_svc;
} seed_kw_t;

/* One live collection handle (DICT/LIST/TREE): a tag + the heap object. */
typedef struct { int type; void *obj; } coll_t;

/* ===================== interp_stmt.c ===================== */

typedef struct { int line; int off; } retaddr_t;

typedef struct {
    char   name[NAME_LEN];
    double limit;
    double step;
    int    line;        // position of the loop body (first thing after FOR...)
    int    off;
} for_rec_t;

// Loop-type tags for EXIT/CONTINUE. The three loop stacks (for_/repeat_/while_)
// don't record their relative nesting order, so EXIT/CONTINUE find the innermost
// loop by comparing the source positions of each stack's active frames instead.
enum { LOOP_FOR = 1, LOOP_REPEAT, LOOP_WHILE };

typedef struct {
    int catch_pc, catch_off;       // where the CATCH handler body begins
    int call_sp;                   // PROC/FN depth the TRY sits at
    int for_sp, repeat_sp, while_sp, case_sp, gosub_sp, fn_ret_sp, local_sp;
    int scratch_base, scratch_top;
} try_rec_t;

// newstyle: 1 = DEF fn/proc NAME (spaced, END fn/proc-terminated); 0 = glued
// DEF FNNAME / DEF PROCNAME (=<expr> / ENDPROC-terminated).
typedef struct { char name[NAME_LEN]; int is_fn; int newstyle; int line; int off; } defrec_t;

/* ===================== interp_events.c ===================== */

/* ON TIMER/PIN/MOUSE/KEY handler records (one per source line). */
typedef struct { int active; char proc[NAME_LEN]; long interval_us; unsigned long long last_us; } ev_timer_t;
typedef struct { int active; char proc[NAME_LEN]; int x, y, b; } ev_mouse_t;
typedef struct { int active; char proc[NAME_LEN]; } ev_key_t;
typedef struct { int active; char proc[NAME_LEN]; int pin, edge, level; } ev_pin_t;

/* ===================== interp_hw.c ===================== */

typedef struct { int freq, vol; long dur_us; } snd_note;   // dur_us < 0 = play forever

/* ===================== interp_pod.c ===================== */

typedef struct {
    int      used;        // slot occupied
    int      resident;    // extension kept loaded (its keywords are registered)
    uint64_t caps;
    uint32_t image_size, split_off, init_size, entry_off, flags;
    char     name[16];    // MARK name=, for PODFREE
    BerryServices svc;      // this POD's capability-gated services table
} pod_slot_t;

typedef struct {
    uint64_t caps;
    uint32_t image_size, split_off, init_size, entry_off, flags, build_epoch;
    uint16_t abi;
    const unsigned char *imag; int imag_len;
    const unsigned char *keyw; int keyw_len;
    const unsigned char *rloc; int rloc_len;
    const unsigned char *mark; int mark_len;
    const unsigned char *need; int need_len;
} pod_image_t;

enum { POD_MAX_ARGV = 40 };

/* ===================== interp_call.c ===================== */

// A record passed as an argument. Records are passed *by reference*: only these
// offsets travel into the callee, so its parameter addresses the caller's own
// field storage and writes through to it.
typedef struct { int rtype, nelem, off_num, off_str; } recref_t;

#endif /* INTERP_TYPES_H */
