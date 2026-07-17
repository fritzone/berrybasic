// ===========================================================================
// BerryBasiC — evaluator support: forward decls, argument & subscript helpers, RND
//
// This file is a fragment of the interpreter and is #included by basic.c.
// It is NOT a standalone translation unit: it shares the single translation
// unit's static state and is compiled only as part of basic.c. Do not add it
// to the build system or compile it on its own.
// ===========================================================================
// ---------------------------------------------------------------------------
// Expression evaluator (recursive descent), now value-typed (number | string).
//   expr  := add (relop add)?          relop result is numeric 0/1
//   add   := term (('+'|'-') term)*    '+' concatenates two strings
//   term  := unary (('*'|'/') unary)*  numeric only
//   unary := ('-'|'+') unary | primary numeric only
//   primary := NUM | STR | VAR | func(...) | '(' expr ')'
// ---------------------------------------------------------------------------

static value_t eval_expr(void);
static void    call_proc(int is_fn, value_t *retval);   // PROC/FN call (defined later)
static void    call_named(int is_fn, const char *name, value_t *retval);
static int     find_fn_def(const char *name);

// Pull a required numeric/string operand, raising TYPE MISMATCH otherwise.
static double need_num(void) {
    value_t v = eval_expr();
    if (g_err) return 0;
    if (v.is_str) { err("Type mismatch: numbers and text can't be mixed"); return 0; }
    return v.num;
}
static value_t need_str(void) {
    value_t v = eval_expr();
    if (g_err) return v_num(0);
    if (!v.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
    return v;
}

// Human-readable name of a token type, for "Expected X" error messages.
static const char *tok_name(int t) {
    switch (t) {
        case T_EOL:   return "end of line";
        case T_NUM:   return "a number";
        case T_STR:   return "a string";
        case T_VAR:   return "a variable name";
        case T_KW:    return "a keyword";
        case T_PLUS:  return "'+'";
        case T_MINUS: return "'-'";
        case T_STAR:  return "'*'";
        case T_SLASH: return "'/'";
        case T_CARET: return "'^'";
        case T_LP:    return "'('";
        case T_RP:    return "')'";
        case T_COMMA: return "','";
        case T_SEMI:  return "';'";
        case T_COLON: return "':'";
        case T_EQ:    return "'='";
        case T_NE:    return "'<>'";
        case T_LT:    return "'<'";
        case T_GT:    return "'>'";
        case T_LE:    return "'<='";
        case T_GE:    return "'>='";
        default:      return "something else";
    }
}

static int expect(int t) {              // consume token t or report what's missing
    if (tok != t) { err2("Expected ", tok_name(t)); return 0; }
    lex_next();
    return 1;
}

static value_t str_in_scratch(const char *src, int len) {
    if (len > MAX_STR) len = MAX_STR;
    char *p = scratch_alloc(len);
    if (!p) return v_num(0);
    for (int i = 0; i < len; i++) p[i] = src[i];
    return v_str(p, len);
}

// Parse "(e1[,e2,...])" into subs[], with tok currently at '('. Returns count.
static int parse_subscripts(int *subs, int *nsub) {
    lex_next();                              // consume '('
    int n = 0;
    for (;;) {
        double v = need_num();
        if (g_err) return 0;
        if (n < MAX_DIMS) subs[n] = (int)v;  // subscripts truncate to integer
        n++;
        if (tok == T_COMMA) { lex_next(); continue; }
        break;
    }
    if (!expect(T_RP)) return 0;
    if (n > MAX_DIMS) { err("Array index out of range"); return 0; }
    *nsub = n;
    return 1;
}

// Resolve an array element reference (tok at '('): parse subscripts, auto-DIM
// to 0..10 per dimension if the array does not exist, and return the pool index
// in *idx and the array in *out.
static int arr_elem(const char *name, int is_str, int *idx, arr_t **out) {
    int subs[MAX_DIMS];
    int nsub;
    if (!parse_subscripts(subs, &nsub)) return 0;
    arr_t *a = arr_find(name);
    if (!a) {
        int counts[MAX_DIMS];
        for (int i = 0; i < nsub; i++) counts[i] = 11;   // default DIM x(10)
        a = arr_create(name, nsub, counts, is_str);
        if (!a) return 0;
    }
    if (a->ndim != nsub) { err("Array index out of range"); return 0; }
    int off = 0;
    for (int i = 0; i < nsub; i++) {
        if (subs[i] < 0 || subs[i] >= a->dim[i]) { err("Array index out of range"); return 0; }
        off = off * a->dim[i] + subs[i];
    }
    *idx = a->off + off;
    *out = a;
    return 1;
}

// ---------------------------------------------------------------------------
// Record field references: p.x, or e(3).x for an array of records.
// ---------------------------------------------------------------------------

// Read a type or field name into `out` and consume it. These names live in
// their own namespace and only ever appear where nothing else is legal (after
// TYPE or AS, or in a field list), so a word that happens to be a keyword -
// POINT, SIZE, TIME - is still a usable name. Field *reads* never come through
// here: the lexer hands those over as T_LABEL, which is never keyword-matched,
// so accepting keywords here is what keeps `TYPE point : size` and `p.size`
// agreeing on the same name. Returns 0 on error, having raised `what`.
static int read_name_word(char *out, const char *what) {
    if (tok == T_VAR) { s_copy(out, tok_var, NAME_LEN); lex_next(); return 1; }
    if (tok == T_KW) {
        const char *s = is_seed_kw(tok_kw) ? seed_kw_name(tok_kw - KW_SEED_DYN)
                                           : kw_spelling(tok_kw);
        if (s) { s_copy(out, s, NAME_LEN); lex_next(); return 1; }
    }
    err(what);
    return 0;
}

// Where a resolved field lives, and what kind it is.
typedef struct { int f; int slot; int is_str; int is_int; } fieldref_t;

// Parse the element selector of a record reference: '(index)' for an array of
// records, nothing at all for a scalar one. On entry the current token is just
// past the variable's name. Returns 0 on error, having raised it.
static int rec_index(const var_t *v, int *e) {
    *e = 0;
    if (tok == T_LP) {
        lex_next();
        double d = need_num();
        if (g_err) return 0;
        if (!expect(T_RP)) return 0;
        *e = (int)d;
    } else if (v->nelem > 1) {
        err("Expected an index like e(3)"); return 0;
    }
    if (*e < 0 || *e >= v->nelem) { err("Array index out of range"); return 0; }
    return 1;
}

// Resolve a field reference. On entry `v` is the record variable and the current
// token is whatever followed its name: an optional '(index)' selecting an
// element, then the '.field' (a T_LABEL - see the lexer). On return the current
// token is past the field name. Returns 0 on error, having raised it.
static int rec_field_ref(const var_t *v, fieldref_t *fr) {
    int e;
    if (!rec_index(v, &e)) return 0;
    if (tok != T_LABEL) { err("Expected a field name like p.x"); return 0; }
    const type_t *ty = &types[v->rtype];
    int f = type_field_in(ty, tok_var);
    if (f < 0) { err("No such field"); return 0; }
    lex_next();
    fr->f      = f;
    fr->is_str = ty->fis_str[f];
    fr->is_int = ty->fis_int[f];
    fr->slot   = fr->is_str ? rec_str_slot(v, e, f) : rec_num_slot(v, e, f);
    return 1;
}

// Read a record field's current value.
static value_t rec_field_get(const var_t *v) {
    fieldref_t fr;
    if (!rec_field_ref(v, &fr)) return v_num(0);
    return fr.is_str ? v_str(rec_strs[fr.slot].sptr, rec_strs[fr.slot].slen)
                     : v_num(rec_nums[fr.slot]);
}

// ---------------------------------------------------------------------------
// Assignment targets: where a statement that fills a variable from somewhere
// other than an expression (READ, INPUT, INPUT#) is being asked to put it.
// ---------------------------------------------------------------------------

// A resolved target: a scalar variable, an array element, or a record field.
// Exactly one of nump/strp is set, according to is_str.
typedef struct {
    int        is_str, is_int;
    double    *nump;
    strdesc_t *strp;
} target_t;

// Parse a target at the current token - name, name(subs), p.field, e(3).field -
// and resolve where its value lives. Returns 0 on error, having raised it.
static int parse_target(target_t *t) {
    if (tok != T_VAR) { err("Expected a variable name"); return 0; }
    char name[NAME_LEN];
    s_copy(name, tok_var, NAME_LEN);
    int isstr = name_is_str(name);
    const var_t *rec = var_lookup(name);
    if (rec && !rec->is_rec) rec = 0;
    lex_next();

    if (rec) {                               // p.x / e(3).x
        fieldref_t fr;
        if (!rec_field_ref(rec, &fr)) return 0;
        t->is_str = fr.is_str; t->is_int = fr.is_int;
        t->nump = fr.is_str ? 0 : &rec_nums[fr.slot];
        t->strp = fr.is_str ? &rec_strs[fr.slot] : 0;
        return 1;
    }
    if (tok == T_LP) {                       // a(3)
        int idx; arr_t *a;
        if (!arr_elem(name, isstr, &idx, &a)) return 0;
        t->is_str = a->is_str; t->is_int = a->is_int;
        t->nump = a->is_str ? 0 : &arr_nums[idx];
        t->strp = a->is_str ? &arr_strs[idx] : 0;
        return 1;
    }
    var_t *v = var_find(name);               // plain variable
    if (!v) return 0;
    t->is_str = v->is_str; t->is_int = v->is_int;
    t->nump = v->is_str ? 0 : &v->num;
    t->strp = v->is_str ? &v->s : 0;
    return 1;
}

static void target_put_num(const target_t *t, double x) {
    if (t->is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
    *t->nump = trunc_int(t->is_int, x);
}
static void target_put_str(const target_t *t, const char *s, int len) {
    if (!t->is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
    str_store_to(t->strp, s, len);
}

static long long time_base = 0;     // TIME (centiseconds) = con_micros()/10000 - time_base
static unsigned long rnd_seed = 22695477UL;
static double rnd_last = 0.0;
static double rnd_float(void) {         // pseudo-random in [0.0, 1.0)
    rnd_seed = rnd_seed * 1103515245UL + 12345UL;
    unsigned long r = (rnd_seed >> 16) & 0x7FFFFFFFUL;
    rnd_last = (double)r / 2147483648.0;   // / 2^31
    return rnd_last;
}

static value_t eval_primary(void);      // a function argument is a factor (primary)
static value_t prim_base(void);         // a primary without the ?/! indirection postfix

// Parse a single function argument as the next factor: a primary expression,
// which is either a parenthesised group or a bare value. This makes the
// single-argument math functions paren-optional, BBC-style (SQR 3 = SQR(3),
// SQR(3) and SQR(X+1) all work, and SQR binds tighter than the operators around
// it, so X * SQR 3 / 6 is X * (SQR 3) / 6).
static double factor_num(void) {
    value_t v = eval_primary();
    if (g_err) return 0;
    if (v.is_str) { err("Type mismatch: numbers and text can't be mixed"); return 0; }
    return v.num;
}

// Parse a file channel operand: the '#' prefix followed by a numeric factor (so
// BGET#ch+1 is (BGET#ch)+1, and BGET#(a+1) uses parentheses). Returns the channel.
static int read_channel(void) {
    if (tok != T_HASH) { err("Expected '#' before a file channel"); return 0; }
    lex_next();
    return (int)factor_num();
}

// Copy a BASIC string value into a NUL-terminated C filename buffer.
static void copy_fname(value_t s, char *out, int outsz) {
    int n = (s.len < outsz - 1) ? s.len : outsz - 1;
    for (int i = 0; i < n; i++) out[i] = s.str[i];
    out[n] = 0;
}

// Directory-scan cursor state, shared by DIROPEN / DIRNEXT and the field words
// DIRNAME$ / DIRSIZE / DIRTYPE / DIRDATE$ / DIRTIME$. Only one scan runs at a
// time; g_dir_valid says whether g_dirent holds a live entry.
static stg_dirent g_dirent;
static int        g_dir_valid;

static void fmt_u2(char *p, int v) { p[0] = '0' + (v / 10) % 10; p[1] = '0' + v % 10; }
static void fmt_date(char *b, int y, int m, int d) {   // "YYYY-MM-DD"
    b[0] = '0' + (y / 1000) % 10; b[1] = '0' + (y / 100) % 10;
    b[2] = '0' + (y / 10) % 10;   b[3] = '0' + y % 10;
    b[4] = '-'; fmt_u2(b + 5, m); b[7] = '-'; fmt_u2(b + 8, d); b[10] = 0;
}
static void fmt_time(char *b, int h, int m) {          // "HH:MM"
    fmt_u2(b, h); b[2] = ':'; fmt_u2(b + 3, m); b[5] = 0;
}

