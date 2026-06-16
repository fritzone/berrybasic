#include <stdint.h>
#include "console.h"
#include "storage.h"
#include "basic.h"

// ===========================================================================
// BerryBasiC - A small BBC-flavoured BASIC interpreter.
//
// Design notes
//   * Pure, freestanding C: no libc and no libm (math is hand-rolled), so the
//     exact same source builds for the bare-metal target and the host harness.
//   * Numbers are double-precision floating point.
//   * Program lines are stored as source text in a sorted table; the lexer
//     re-tokenises on the fly when a line runs. Crunching to byte tokens is a
//     later optimisation.
//   * Errors set a flag that unwinds the current statement/RUN without longjmp.
//
// Statements: PRINT LET INPUT DIM GOTO GOSUB RETURN FOR/NEXT IF..THEN REM END
// RUN LIST NEW. Functions: ABS INT SGN SQR SIN COS TAN ATN LOG EXP RND LEN ASC
// VAL CHR$ STR$ LEFT$ RIGHT$ MID$, constant PI. Operators ^ * / + -, relational
// = <> < > <= >=, string concatenation. Strings use a GC'd heap.
// ===========================================================================

#define LINE_LEN     128     // maximum length of a line
#define MAX_LINES   8192     // total length of a BASIC program
#define MAX_VARS     512     // total number of allowed variables in a program
#define NAME_LEN       8     // variable name incl. trailing '$' for strings
#define MAX_STR      255     // classic BASIC maximum string length
#define GCHEAP_SIZE 16384    // bytes of string storage for variables (GC'd)
#define SCRATCH_SIZE 4096    // per-statement scratch for string temporaries

#define BAS_PI     3.14159265358979323846
#define BAS_HALFPI 1.57079632679489661923
#define BAS_TWOPI  6.28318530717958647692
#define BAS_LN2    0.69314718055994530942

// ---------------------------------------------------------------------------
// Tiny freestanding helpers (no string.h on the target)
// ---------------------------------------------------------------------------

static int  is_space(char c) { return c == ' ' || c == '\t'; }
static int  is_digit(char c) { return c >= '0' && c <= '9'; }
static int  is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int  is_alnum(char c) { return is_alpha(c) || is_digit(c) || c == '_'; }
static char up(char c)       { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static void s_copy(char *d, const char *s, int max) {
    int i = 0;
    for (; s[i] && i < max - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static int s_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

// True if the first n characters of a equal b (b must be at least n chars).
static int s_eqn(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

// ---------------------------------------------------------------------------
// Error state
// ---------------------------------------------------------------------------

static int g_err     = 0;    // set by err(); unwinds the current operation
static int g_runline = -1;   // line number currently executing, or -1 immediate

// Print the " (line N)" suffix and the trailing newline of an error report.
static void err_tail(void) {
    if (g_runline >= 0) {
        con_puts(" (line ");
        char buf[12]; int n = 0; long v = g_runline;
        if (v == 0) buf[n++] = '0';
        while (v) { buf[n++] = (char)('0' + v % 10); v /= 10; }
        while (n) con_putc(buf[--n]);
        con_putc(')');
    }
    con_putc('\n');
}

// Report an error and unwind the current operation; only the first one sticks.
// e.g. "Syntax error (line 30)".
static void err(const char *msg) {
    if (g_err) return;       // keep the first error
    g_err = 1;
    con_putc('\n');
    con_puts(msg);
    err_tail();
}

// Two-part error, for a fixed prefix plus a dynamic name, e.g.
// err2("Expected ", "')'") -> "Expected ')' (line 30)".
static void err2(const char *a, const char *b) {
    if (g_err) return;
    g_err = 1;
    con_putc('\n');
    con_puts(a);
    con_puts(b);
    err_tail();
}

static void con_putn(long v) {
    char buf[24];
    int  n = 0;
    unsigned long u;
    if (v < 0) { con_putc('-'); u = (unsigned long)(-(v + 1)) + 1; }
    else       { u = (unsigned long)v; }
    if (u == 0) { con_putc('0'); return; }
    while (u) { buf[n++] = (char)('0' + u % 10); u /= 10; }
    while (n) con_putc(buf[--n]);
}

static void con_putsn(const char *s, int len) {     // strings are not NUL-terminated
    for (int i = 0; i < len; i++) con_putc(s[i]);
}

// ---------------------------------------------------------------------------
// Floating-point helpers (no libm: only +-*/ and comparisons)
// ---------------------------------------------------------------------------

static double dfloor(double x) {
    if (x >= 9.2e18 || x <= -9.2e18) return x;   // out of long long range: already integral
    long long i = (long long)x;                  // truncates toward zero
    if ((double)i > x) i--;
    return (double)i;
}

static double dround(double x) { return dfloor(x + 0.5); }

static double dsqrt(double x) {
    if (x <= 0) return 0;
    double g = x;                                // Newton's method
    for (int i = 0; i < 40; i++) g = 0.5 * (g + x / g);
    return g;
}

static double dexp(double x) {
    if (x > 709.0)  return 1.0e308 * 10.0;       // overflow -> +inf
    if (x < -745.0) return 0.0;                  // underflow -> 0
    double k = dround(x / BAS_LN2);              // x = k*ln2 + r, |r| <= ln2/2
    double r = x - k * BAS_LN2;
    double term = 1, sum = 1;                    // e^r via Taylor
    for (int n = 1; n < 18; n++) { term *= r / n; sum += term; }
    int ki = (int)k;                             // multiply by 2^k
    if (ki >= 0) for (int i = 0; i < ki; i++) sum *= 2.0;
    else         for (int i = 0; i < -ki; i++) sum *= 0.5;
    return sum;
}

static double dlog(double x) {                   // natural log, x>0 (caller checks)
    if (x <= 0) return 0;
    int e = 0;
    while (x >= 2.0) { x *= 0.5; e++; }
    while (x < 1.0)  { x *= 2.0; e--; }
    if (x > 1.41421356237) { x *= 0.5; e++; }    // center on 1: x in [0.707,1.414)
    double t = (x - 1) / (x + 1), t2 = t * t, term = t, sum = t;
    for (int n = 1; n < 20; n++) { term *= t2; sum += term / (2 * n + 1); }
    return 2 * sum + e * BAS_LN2;                // log(x) = 2*atanh(t) + e*ln2
}

static double dsin(double x) {
    double k = dround(x / BAS_TWOPI);  x -= k * BAS_TWOPI;        // x in [-pi,pi]
    int    n = (int)dround(x / BAS_HALFPI);
    double r = x - n * BAS_HALFPI;                                // r in [-pi/4,pi/4]
    double r2 = r * r;
    double st = r,  ss = r;                       // sin(r)
    for (int i = 1; i < 8; i++) { st *= -r2 / ((2 * i) * (2 * i + 1)); ss += st; }
    double ct = 1,  cc = 1;                       // cos(r)
    for (int i = 1; i < 8; i++) { ct *= -r2 / ((2 * i - 1) * (2 * i)); cc += ct; }
    switch (((n % 4) + 4) % 4) {
        case 0:  return ss;
        case 1:  return cc;
        case 2:  return -ss;
        default: return -cc;
    }
}

static double dcos(double x) { return dsin(x + BAS_HALFPI); }
static double dtan(double x) { double c = dcos(x); if (c == 0) c = 1e-300; return dsin(x) / c; }

static double datan(double x) {                   // result in (-pi/2, pi/2)
    int sign = 1; if (x < 0) { sign = -1; x = -x; }
    int big = 0; if (x > 1) { big = 1; x = 1 / x; }
    int red = 0;                                  // half-angle: atan(x)=2*atan(x/(1+sqrt(1+x^2)))
    while (x > 0.2) { x = x / (1 + dsqrt(1 + x * x)); red++; }
    double x2 = x * x, term = x, s = x;           // Taylor
    for (int n = 1; n < 12; n++) { term *= -x2; s += term / (2 * n + 1); }
    double r = s * (double)(1 << red);
    if (big) r = BAS_HALFPI - r;
    return sign * r;
}

static double dpow(double a, double b) {          // a ^ b
    if (b == 0) return 1;
    if (a == 0) return 0;
    if (a > 0)  return dexp(b * dlog(a));
    double rb = dround(b);                         // negative base: integer exponent only
    if (rb != b) { err("Invalid argument"); return 0; }
    double r = dexp(rb * dlog(-a));
    return ((long)rb & 1) ? -r : r;
}

// Format v into out[] (needs ~32 bytes) as BASIC would, returning the length.
// Whole numbers print without a decimal point; otherwise up to 9 significant
// digits with trailing zeros trimmed; very large/small values use E notation.
static int dbl_to_str(char *out, double v) {
    int n = 0;
    if (v != v) { out[0] = 'N'; out[1] = 'A'; out[2] = 'N'; return 3; }   // NaN
    if (v == 0) { out[0] = '0'; return 1; }
    if (v < 0) { out[n++] = '-'; v = -v; }
    if (v > 1.0e308) { out[n++] = 'I'; out[n++] = 'N'; out[n++] = 'F'; return n; }

    int e = 0;                                   // decimal exponent: 1 <= m < 10
    double m = v;
    while (m >= 10.0) { m /= 10.0; e++; }
    while (m < 1.0)   { m *= 10.0; e--; }

    long long mant = (long long)(m * 1.0e8 + 0.5);   // 9 significant digits
    if (mant >= 1000000000LL) { mant /= 10; e++; }   // rounding rolled to 10.0
    char d[9];
    for (int i = 8; i >= 0; i--) { d[i] = (char)('0' + mant % 10); mant /= 10; }
    int ndig = 9;
    while (ndig > 1 && d[ndig - 1] == '0') ndig--;

    if (e >= -4 && e < 9) {                       // fixed-point
        if (e >= 0) {
            int ip = e + 1;                       // digits before the point
            for (int i = 0; i < ndig; i++) {
                if (i == ip) out[n++] = '.';
                out[n++] = d[i];
            }
            for (int i = ndig; i < ip; i++) out[n++] = '0';
        } else {
            out[n++] = '0'; out[n++] = '.';
            for (int i = 0; i < -e - 1; i++) out[n++] = '0';
            for (int i = 0; i < ndig; i++) out[n++] = d[i];
        }
    } else {                                      // scientific
        out[n++] = d[0];
        if (ndig > 1) { out[n++] = '.'; for (int i = 1; i < ndig; i++) out[n++] = d[i]; }
        out[n++] = 'E';
        int ex = e;
        if (ex < 0) { out[n++] = '-'; ex = -ex; } else out[n++] = '+';
        if (ex < 10) out[n++] = '0';              // at least two exponent digits
        char eb[3]; int en = 0;
        if (ex == 0) eb[en++] = '0';
        while (ex) { eb[en++] = (char)('0' + ex % 10); ex /= 10; }
        while (en) out[n++] = eb[--en];
    }
    return n;
}


// Parse a number from s[0..len). *consumed receives how many chars were used.
static double parse_double(const char *s, int len, int *consumed) {
    int i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    int sign = 1;
    if (i < len && (s[i] == '-' || s[i] == '+')) { if (s[i] == '-') sign = -1; i++; }
    double val = 0; int any = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') { val = val * 10 + (s[i] - '0'); i++; any = 1; }
    if (i < len && s[i] == '.') {
        i++;
        double f = 0, sc = 1;
        while (i < len && s[i] >= '0' && s[i] <= '9') { f = f * 10 + (s[i] - '0'); sc *= 10; i++; any = 1; }
        val += f / sc;
    }
    if (any && i < len && (s[i] == 'E' || s[i] == 'e')) {
        int j = i + 1, es = 1;
        if (j < len && (s[j] == '+' || s[j] == '-')) { if (s[j] == '-') es = -1; j++; }
        int ev = 0, ed = 0;
        while (j < len && s[j] >= '0' && s[j] <= '9') { ev = ev * 10 + (s[j] - '0'); j++; ed = 1; }
        if (ed) {
            double p = 1; for (int k = 0; k < ev; k++) p *= 10;
            val = es < 0 ? val / p : val * p;
            i = j;
        }
    }
    if (consumed) *consumed = i;
    return sign * val;
}

// ---------------------------------------------------------------------------
// Program storage: sorted-by-line-number table of source lines
// ---------------------------------------------------------------------------

typedef struct { int num; char text[LINE_LEN]; } progline_t;

static progline_t prog[MAX_LINES];
static int        prog_n = 0;

static int line_is_blank(const char *t) {
    for (; *t; t++) if (!is_space(*t)) return 0;
    return 1;
}

static void prog_store(int num, const char *text) {
    int i = 0;
    while (i < prog_n && prog[i].num < num) i++;

    if (i < prog_n && prog[i].num == num) {       // replace or delete existing
        if (line_is_blank(text)) {                // "10" alone deletes line 10
            for (int j = i; j < prog_n - 1; j++) prog[j] = prog[j + 1];
            prog_n--;
        } else {
            s_copy(prog[i].text, text, LINE_LEN);
        }
        return;
    }
    if (line_is_blank(text)) return;              // delete of nonexistent line
    if (prog_n >= MAX_LINES) { err("Out of memory"); return; }

    for (int j = prog_n; j > i; j--) prog[j] = prog[j - 1];  // insert, keep sorted
    prog[i].num = num;
    s_copy(prog[i].text, text, LINE_LEN);
    prog_n++;
}

static int find_line_index(int num) {
    for (int i = 0; i < prog_n; i++) if (prog[i].num == num) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Runtime values: a value is either a number (long) or a string (ptr+len).
// String bytes are NOT NUL-terminated; the length is authoritative.
// ---------------------------------------------------------------------------

typedef struct {
    int    is_str;
    double num;
    char  *str;
    int    len;
} value_t;

static value_t v_num(double n)          { value_t v; v.is_str = 0; v.num = n; v.str = 0; v.len = 0; return v; }
static value_t v_str(char *p, int len)  { value_t v; v.is_str = 1; v.num = 0; v.str = p; v.len = len; return v; }

// A string value living in the GC heap: bytes pointer + length.
typedef struct { char *sptr; int slen; } strdesc_t;

static int name_is_str(const char *name) {
    int i = 0; while (name[i]) i++;
    return i > 0 && name[i - 1] == '$';
}
static int name_is_int(const char *name) {           // A%, COUNT% etc. are integers
    int i = 0; while (name[i]) i++;
    return i > 0 && name[i - 1] == '%';
}
static double trunc_int(int is_int, double x) {      // BBC % vars truncate toward zero
    return is_int ? (double)(long)x : x;
}

// ---------------------------------------------------------------------------
// Scalar variables. A variable holds either a number or a string descriptor;
// the trailing '$' in the name selects which (A and A$ are distinct).
// ---------------------------------------------------------------------------

typedef struct {
    char      name[NAME_LEN];
    int       is_str;
    int       is_int;        // % suffix: value is kept truncated to an integer
    double    num;
    strdesc_t s;             // string bytes live in the GC heap (gcheap)
} var_t;

static var_t vars[MAX_VARS];
static int   var_n = 0;

static var_t *var_find(const char *name) {
    for (int i = 0; i < var_n; i++)
        if (s_eq(vars[i].name, name)) return &vars[i];
    if (var_n >= MAX_VARS) { err("Out of memory"); return 0; }
    var_t *v = &vars[var_n++];
    s_copy(v->name, name, NAME_LEN);
    v->is_str = name_is_str(name);
    v->is_int = name_is_int(name);
    v->num = 0; v->s.sptr = 0; v->s.slen = 0;
    return v;
}

// ---------------------------------------------------------------------------
// Arrays. Element storage is bump-allocated from fixed pools (no malloc):
// numeric elements from arr_nums[], string elements from arr_strs[] (each a
// strdesc into the GC heap). String array elements are GC roots too.
// ---------------------------------------------------------------------------

#define MAX_ARRAYS      16
#define MAX_DIMS         3
#define ARR_NUM_POOL  4096      // numeric elements (longs)
#define ARR_STR_POOL   512      // string elements (descriptors)

typedef struct {
    char name[NAME_LEN];
    int  is_str;
    int  is_int;               // % suffix: integer elements
    int  ndim;
    int  dim[MAX_DIMS];        // element count per dimension (DIM value + 1)
    int  total;                // product of dims
    int  off;                  // base index into arr_nums[] or arr_strs[]
} arr_t;

static arr_t     arrs[MAX_ARRAYS];
static int       arr_n = 0;
static double    arr_nums[ARR_NUM_POOL];
static int       arr_nums_top = 0;
static strdesc_t arr_strs[ARR_STR_POOL];
static int       arr_strs_top = 0;

static arr_t *arr_find(const char *name) {
    for (int i = 0; i < arr_n; i++)
        if (s_eq(arrs[i].name, name)) return &arrs[i];
    return 0;
}

static arr_t *arr_create(const char *name, int ndim, const int *counts, int is_str) {
    if (arr_n >= MAX_ARRAYS) { err("Out of memory"); return 0; }
    if (ndim < 1 || ndim > MAX_DIMS) { err("Array index out of range"); return 0; }
    int total = 1;
    for (int i = 0; i < ndim; i++) total *= counts[i];
    arr_t *a = &arrs[arr_n];
    s_copy(a->name, name, NAME_LEN);
    a->is_str = is_str;
    a->is_int = name_is_int(name);
    a->ndim = ndim;
    a->total = total;
    for (int i = 0; i < ndim; i++) a->dim[i] = counts[i];
    if (is_str) {
        if (arr_strs_top + total > ARR_STR_POOL) { err("Out of memory"); return 0; }
        a->off = arr_strs_top;
        for (int k = 0; k < total; k++) { arr_strs[a->off + k].sptr = 0; arr_strs[a->off + k].slen = 0; }
        arr_strs_top += total;
    } else {
        if (arr_nums_top + total > ARR_NUM_POOL) { err("Out of memory"); return 0; }
        a->off = arr_nums_top;
        for (int k = 0; k < total; k++) arr_nums[a->off + k] = 0;
        arr_nums_top += total;
    }
    arr_n++;
    return a;
}

// ---------------------------------------------------------------------------
// String storage.
//   * gcheap[]  holds the bytes of string *variables and array elements*;
//     compacted by gc().
//   * scratch[] holds short-lived temporaries (literals, concatenation and
//     function results) produced while evaluating one statement; it is rewound
//     to empty at the start of every statement, so its contents never need GC.
//
// gc() is only ever reached from a string store, where the source bytes have
// already been staged into a private buffer. No temporary in scratch is ever a
// GC root, and no live value points into gcheap across a store, so compaction
// can freely relocate strings.
// ---------------------------------------------------------------------------

static char gcheap[GCHEAP_SIZE];
static int  gcheap_top = 0;

static char scratch[SCRATCH_SIZE];
static int  scratch_top = 0;
static int  scratch_base = 0;   // PROC/FN bodies rewind here, preserving caller temporaries

static void scratch_reset(void) { scratch_top = scratch_base; }

static char *scratch_alloc(int n) {
    if (scratch_top + n > SCRATCH_SIZE) { err("Expression too complex"); return 0; }
    char *p = &scratch[scratch_top];
    scratch_top += n;
    return p;
}

// Saved variable values for PROC/FN parameters and LOCAL declarations. These
// hold string descriptors into the GC heap, so they must be GC roots too.
#define LOCAL_MAX 96
typedef struct { var_t *slot; var_t old; } localsave_t;
static localsave_t local_stack[LOCAL_MAX];
static int         local_sp = 0;

// Mark-compact the GC heap: slide every live string down to remove the gaps
// left by overwritten/temporary strings, fixing up the descriptors. Roots are
// scalar string variables, string-array elements, and saved locals.
static strdesc_t *gc_roots[MAX_VARS + ARR_STR_POOL + LOCAL_MAX];

static void gc(void) {
    int n = 0;
    for (int i = 0; i < var_n; i++)
        if (vars[i].is_str && vars[i].s.slen > 0) gc_roots[n++] = &vars[i].s;
    for (int i = 0; i < arr_n; i++)
        if (arrs[i].is_str)
            for (int k = 0; k < arrs[i].total; k++) {
                strdesc_t *d = &arr_strs[arrs[i].off + k];
                if (d->slen > 0) gc_roots[n++] = d;
            }
    for (int i = 0; i < local_sp; i++)
        if (local_stack[i].old.is_str && local_stack[i].old.s.slen > 0)
            gc_roots[n++] = &local_stack[i].old.s;
    for (int i = 1; i < n; i++) {                // insertion sort by sptr
        strdesc_t *k = gc_roots[i];
        int j = i - 1;
        while (j >= 0 && gc_roots[j]->sptr > k->sptr) { gc_roots[j + 1] = gc_roots[j]; j--; }
        gc_roots[j + 1] = k;
    }
    int top = 0;
    for (int i = 0; i < n; i++) {
        char *src = gc_roots[i]->sptr;
        int   len = gc_roots[i]->slen;
        char *dst = &gcheap[top];
        if (dst != src) for (int k = 0; k < len; k++) dst[k] = src[k];  // dst <= src
        gc_roots[i]->sptr = dst;
        top += len;
    }
    gcheap_top = top;
}

static char *gc_alloc(int n) {
    if (gcheap_top + n > GCHEAP_SIZE) gc();
    if (gcheap_top + n > GCHEAP_SIZE) { err("Out of memory"); return 0; }
    char *p = &gcheap[gcheap_top];
    gcheap_top += n;
    return p;
}

// Store len bytes from src into the string descriptor d. src may point anywhere
// (scratch, program text, or another string in gcheap), so it is staged into a
// private buffer before gc_alloc(), which may relocate gcheap.
static void str_store_to(strdesc_t *d, const char *src, int len) {
    static char stage[MAX_STR];
    if (len > MAX_STR) { err("Text string is too long"); return; }
    for (int i = 0; i < len; i++) stage[i] = src[i];
    char *dst = gc_alloc(len);
    if (!dst) return;
    for (int i = 0; i < len; i++) dst[i] = stage[i];
    d->sptr = dst; d->slen = len;
}

static void str_store(var_t *v, const char *src, int len) {
    str_store_to(&v->s, src, len);
    if (!g_err) v->is_str = 1;
}

static void clear_vars(void) {       // BASIC CLR: scalars + arrays + string heap
    var_n = 0;
    gcheap_top = 0;
    arr_n = 0;
    arr_nums_top = 0;
    arr_strs_top = 0;
}

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

enum {
    T_EOL, T_NUM, T_STR, T_VAR, T_KW, T_LABEL,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_CARET,
    T_LP, T_RP, T_COMMA, T_SEMI, T_COLON, T_SQUOTE,
    T_EQ, T_NE, T_LT, T_GT, T_LE, T_GE
};

enum {
    // Keywords
    KW_PRINT, KW_LET, KW_GOTO, KW_IF, KW_THEN,
    KW_REM, KW_END, KW_RUN, KW_LIST, KW_NEW,
    KW_FOR, KW_TO, KW_STEP, KW_NEXT, KW_GOSUB, KW_RETURN, KW_INPUT, KW_DIM, KW_PI,
    KW_REPEAT, KW_UNTIL, KW_ELSE, KW_CLS, KW_COLOUR,
    KW_WHILE, KW_ENDWHILE, KW_ENDIF,                  // structured loops / block IF
    KW_CASE, KW_OF, KW_WHEN, KW_OTHERWISE, KW_ENDCASE, // CASE selection
    KW_DEF, KW_PROC, KW_FN, KW_ENDPROC, KW_LOCAL,     // procedures & functions
    KW_DIV, KW_MOD, KW_AND, KW_OR, KW_EOR, KW_NOT,    // operator keywords
    KW_ON, KW_DATA, KW_READ, KW_RESTORE, KW_STOP, KW_VDU, KW_TIME,  // statements
    KW_MODE, KW_GCOL, KW_PLOT, KW_MOVE, KW_DRAW, KW_CLG,            // graphics statements
    KW_SAVE, KW_LOAD, KW_CAT, KW_DIR, KW_DELETE,                    // storage statements
    KW_AUTO, KW_RENUMBER, KW_EDIT,                                  // editor commands
    KW_TRUE, KW_FALSE, KW_POS, KW_VPOS,               // parenless value keywords
    // Functions from the "standard library"
    KW_ABS, KW_INT, KW_SGN, KW_SQR, KW_SIN, KW_COS, KW_TAN, KW_ATN,
    KW_LOG, KW_EXP, KW_DEG, KW_RAD, KW_ACS, KW_ASN,
    KW_RND, KW_LEN, KW_ASC, KW_VAL, KW_INSTR, KW_GET, KW_INKEY,
    KW_CHRS, KW_STRS, KW_LEFTS, KW_RIGHTS, KW_MIDS, KW_STRINGS, KW_GETS, KW_INKEYS,
    KW_POINT,                                         // POINT(x,y) graphics function
    KW_SHL, KW_SHR, KW_ASR, KW_ROL, KW_ROR,           // bitwise shift / rotate functions
    KW__FIRST_FUNC = KW_ABS, KW__LAST_FUNC = KW_ROR,
    KW_TAB, KW_SPC                                    // PRINT-only modifiers
};

static int is_func_kw(int id) { return id >= KW__FIRST_FUNC && id <= KW__LAST_FUNC; }

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
    { "CLS",   KW_CLS   }, { "COLOUR", KW_COLOUR }, { "COLOR", KW_COLOUR },
    { "DEF",   KW_DEF   }, { "ENDPROC", KW_ENDPROC }, { "LOCAL", KW_LOCAL },
    { "DIV",   KW_DIV   }, { "MOD",  KW_MOD   }, { "AND",    KW_AND    },
    { "OR",    KW_OR    }, { "EOR",  KW_EOR   }, { "NOT",    KW_NOT    },
    { "ON",    KW_ON    }, { "DATA", KW_DATA  }, { "READ",   KW_READ   },
    { "RESTORE", KW_RESTORE }, { "STOP", KW_STOP }, { "VDU",  KW_VDU    },
    { "MODE",  KW_MODE  }, { "GCOL", KW_GCOL  }, { "PLOT",   KW_PLOT   },
    { "MOVE",  KW_MOVE  }, { "DRAW", KW_DRAW  }, { "CLG",    KW_CLG    },
    { "POINT", KW_POINT },
    { "SAVE",  KW_SAVE  }, { "LOAD", KW_LOAD  }, { "CAT",    KW_CAT    },
    { "DIR",   KW_DIR   }, { "DELETE", KW_DELETE },
    { "AUTO",  KW_AUTO  }, { "RENUMBER", KW_RENUMBER }, { "EDIT", KW_EDIT },
    { "TIME",  KW_TIME  }, { "TRUE", KW_TRUE  }, { "FALSE",  KW_FALSE  },
    { "POS",   KW_POS   }, { "VPOS", KW_VPOS  },
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
    { "SHL",   KW_SHL   }, { "SHR",  KW_SHR   }, { "ASR",    KW_ASR    },
    { "ROL",   KW_ROL   }, { "ROR",  KW_ROR   },
    { "TAB",   KW_TAB   }, { "SPC",  KW_SPC   },
};
static const int kwcount = (int)(sizeof(kwtab) / sizeof(kwtab[0]));

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
    // target). A leading '.' followed by a digit is still a number (handled below).
    if (c == '.' && is_alpha(lx[1])) {
        lx++;
        char id[NAME_LEN]; int n = 0;
        while (is_alnum(*lx)) { if (n < NAME_LEN - 1) id[n++] = up(*lx); lx++; }
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
        // BBC PROCname / FNname have no space; the name follows the keyword.
        if (id[0]=='P'&&id[1]=='R'&&id[2]=='O'&&id[3]=='C'&&id[4]) {
            tok = T_KW; tok_kw = KW_PROC; s_copy(tok_var, id + 4, NAME_LEN); return;
        }
        if (id[0]=='F'&&id[1]=='N'&&id[2]) {
            tok = T_KW; tok_kw = KW_FN; s_copy(tok_var, id + 2, NAME_LEN); return;
        }
        for (int i = 0; i < kwcount; i++)
            if (s_eq(id, kwtab[i].name)) { tok = T_KW; tok_kw = kwtab[i].id; return; }
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
        default: err("I don't recognise that character"); tok = T_EOL; return;
    }
}

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

static value_t eval_function(int fn) {
    lex_next();                         // consume function name

    // Single-argument math functions: paren-optional, argument is the next factor.
    switch (fn) {
        case KW_ABS: { double x = factor_num(); return v_num(x < 0 ? -x : x); }
        case KW_INT: return v_num(dfloor(factor_num()));
        case KW_SGN: { double x = factor_num(); return v_num((x > 0) - (x < 0)); }
        case KW_SQR: { double x = factor_num();
                       if (x < 0) { err("Invalid argument"); return v_num(0); }
                       return v_num(dsqrt(x)); }
        case KW_SIN: return v_num(dsin(factor_num()));
        case KW_COS: return v_num(dcos(factor_num()));
        case KW_TAN: return v_num(dtan(factor_num()));
        case KW_ATN: return v_num(datan(factor_num()));
        case KW_DEG: return v_num(factor_num() * 180.0 / BAS_PI);
        case KW_RAD: return v_num(factor_num() * BAS_PI / 180.0);
        case KW_EXP: return v_num(dexp(factor_num()));
        case KW_LOG: { double x = factor_num();
                       if (x <= 0) { err("Invalid argument"); return v_num(0); }
                       return v_num(dlog(x)); }
        case KW_ASN: { double x = factor_num();         // arcsin via atan
                       if (x < -1 || x > 1) { err("Invalid argument"); return v_num(0); }
                       return v_num(datan(x / dsqrt(1 - x * x))); }
        case KW_ACS: { double x = factor_num();         // arccos = pi/2 - arcsin
                       if (x < -1 || x > 1) { err("Invalid argument"); return v_num(0); }
                       return v_num(BAS_HALFPI - datan(x / dsqrt(1 - x * x))); }
    }

    // All other functions keep the parenthesised form: FUNC(arg[,arg...]).
    if (!expect(T_LP)) return v_num(0);
    value_t result = v_num(0);
    switch (fn) {
        case KW_INKEY:  { int n = (int)need_num(); result = v_num((double)con_inkey(n)); break; }
        case KW_INKEYS: { int n = (int)need_num(); int k = con_inkey(n);
                          if (k < 0) result = v_str(0, 0);   // timeout -> empty string
                          else { char c = (char)k; result = str_in_scratch(&c, 1); }
                          break; }
        case KW_RND: { double x = need_num();         // BBC: RND(1)->[0,1), RND(n>1)->1..n
                       double res;
                       if (x == 1)      res = rnd_float();
                       else if (x == 0) res = rnd_last;
                       else if (x < 0)  { rnd_seed = (unsigned long)(-x) | 1UL; res = x; }
                       else             res = (double)(1 + (long)(rnd_float() * x));
                       result = v_num(res); break; }
        case KW_LEN: { value_t s = need_str(); result = v_num(s.len); break; }
        case KW_ASC: { value_t s = need_str();
                       if (s.len == 0) { err("Invalid argument"); break; }
                       result = v_num((unsigned char)s.str[0]); break; }
        case KW_VAL: { value_t s = need_str();
                       result = v_num(parse_double(s.str, s.len, 0)); break; }
        case KW_POINT: { int px = (int)need_num(); if (!expect(T_COMMA)) break;
                         int py = (int)need_num(); if (g_err) break;
                         result = v_num((double)con_point(px, py)); break; }
        case KW_INSTR: { value_t a = need_str(); if (!expect(T_COMMA)) break;
                         value_t b = need_str(); if (g_err) break;
                         int start = 0;
                         if (tok == T_COMMA) { lex_next(); start = (int)need_num() - 1; if (g_err) break; }
                         if (start < 0) start = 0;
                         int pos = 0;                       // 1-based, 0 = not found
                         for (int i = start; i + b.len <= a.len; i++) {
                             int j = 0;
                             while (j < b.len && a.str[i + j] == b.str[j]) j++;
                             if (j == b.len) { pos = i + 1; break; }
                         }
                         result = v_num(pos); break; }
        case KW_CHRS: { int x = (int)need_num();
                        char c = (char)(x & 0xFF);
                        result = str_in_scratch(&c, 1); break; }
        case KW_STRS: { double x = need_num();
                        char buf[40]; int n = dbl_to_str(buf, x);
                        result = str_in_scratch(buf, n); break; }
        case KW_LEFTS: { value_t s = need_str(); if (!expect(T_COMMA)) break;
                         int n = (int)need_num(); if (g_err) break;
                         if (n < 0) n = 0;
                         if (n > s.len) n = s.len;
                         result = str_in_scratch(s.str, n); break; }
        case KW_RIGHTS: { value_t s = need_str(); if (!expect(T_COMMA)) break;
                          int n = (int)need_num(); if (g_err) break;
                          if (n < 0) n = 0;
                          if (n > s.len) n = s.len;
                          result = str_in_scratch(s.str + (s.len - n), n); break; }
        case KW_MIDS: { value_t s = need_str(); if (!expect(T_COMMA)) break;
                        int start = (int)need_num(); if (g_err) break;
                        int cnt = s.len;                       // default: to end
                        if (tok == T_COMMA) { lex_next(); cnt = (int)need_num(); if (g_err) break; }
                        if (start < 1) start = 1;
                        int from = start - 1;                  // 1-based
                        if (from > s.len) from = s.len;
                        int avail = s.len - from;
                        if (cnt < 0) cnt = 0;
                        if (cnt > avail) cnt = avail;
                        result = str_in_scratch(s.str + from, cnt); break; }
        case KW_STRINGS: { int rep = (int)need_num(); if (!expect(T_COMMA)) break;
                           value_t s = need_str(); if (g_err) break;
                           if (rep < 0) rep = 0;
                           if (rep * s.len > MAX_STR) { err("Text string is too long"); break; }
                           char *p = scratch_alloc(rep * s.len);
                           if (!p) break;
                           for (int i = 0; i < rep; i++)
                               for (int j = 0; j < s.len; j++) p[i * s.len + j] = s.str[j];
                           result = v_str(p, rep * s.len); break; }
        // Bitwise shift / rotate on 32-bit integers. SHL/SHR shift in zeros
        // (SHR is logical/unsigned); ASR is an arithmetic right shift that keeps
        // the sign bit; ROL/ROR rotate within 32 bits.
        case KW_SHL: { unsigned x = (unsigned)(int)need_num(); if (!expect(T_COMMA)) break;
                       int n = (int)need_num(); if (g_err) break;
                       if (n < 0) { err("Invalid argument"); break; }
                       result = v_num((double)(int)(n >= 32 ? 0u : x << n)); break; }
        case KW_SHR: { unsigned x = (unsigned)(int)need_num(); if (!expect(T_COMMA)) break;
                       int n = (int)need_num(); if (g_err) break;
                       if (n < 0) { err("Invalid argument"); break; }
                       result = v_num((double)(int)(n >= 32 ? 0u : x >> n)); break; }
        case KW_ASR: { int x = (int)need_num(); if (!expect(T_COMMA)) break;
                       int n = (int)need_num(); if (g_err) break;
                       if (n < 0) { err("Invalid argument"); break; }
                       if (n > 31) n = 31;
                       result = v_num((double)(x >> n)); break; }
        case KW_ROL: { unsigned x = (unsigned)(int)need_num(); if (!expect(T_COMMA)) break;
                       int n = (int)need_num() & 31; if (g_err) break;
                       result = v_num((double)(int)((x << n) | (x >> ((32 - n) & 31)))); break; }
        case KW_ROR: { unsigned x = (unsigned)(int)need_num(); if (!expect(T_COMMA)) break;
                       int n = (int)need_num() & 31; if (g_err) break;
                       result = v_num((double)(int)((x >> n) | (x << ((32 - n) & 31)))); break; }
        default: err("Syntax error in expression"); break;
    }
    if (!g_err) expect(T_RP);
    return result;
}

static value_t eval_primary(void) {
    if (g_err) return v_num(0);
    if (tok == T_NUM) { value_t v = v_num(tok_num); lex_next(); return v; }
    if (tok == T_STR) {
        int n = 0; while (tok_str[n]) n++;
        value_t v = str_in_scratch(tok_str, n);     // copy: tok_str is reused by the lexer
        lex_next();
        return v;
    }
    if (tok == T_VAR) {
        char name[NAME_LEN];
        s_copy(name, tok_var, NAME_LEN);
        int isstr = name_is_str(name);
        lex_next();
        if (tok == T_LP) {                       // array element
            int idx; arr_t *a;
            if (!arr_elem(name, isstr, &idx, &a)) return v_num(0);
            return a->is_str ? v_str(arr_strs[idx].sptr, arr_strs[idx].slen)
                             : v_num(arr_nums[idx]);
        }
        var_t *v = var_find(name);
        return (v && v->is_str) ? v_str(v->s.sptr, v->s.slen)
                                : v_num(v ? v->num : 0);
    }
    if (tok == T_KW) {                       // parenless value keywords
        switch (tok_kw) {
            case KW_PI:    lex_next(); return v_num(BAS_PI);
            case KW_TRUE:  lex_next(); return v_num(-1.0);
            case KW_FALSE: lex_next(); return v_num(0.0);
            case KW_POS:   lex_next(); return v_num(con_pos());
            case KW_VPOS:  lex_next(); return v_num(con_vpos());
            case KW_TIME:  lex_next();
                           return v_num((double)((long long)(con_micros() / 10000ULL) - time_base));
            case KW_FN:  { value_t v = v_num(0); call_proc(1, &v); return v; }
            case KW_GET:   lex_next(); return v_num((double)con_getkey());
            case KW_GETS:{ lex_next(); char ch = (char)con_getkey(); return str_in_scratch(&ch, 1); }
        }
    }
    if (tok == T_KW && is_func_kw(tok_kw)) return eval_function(tok_kw);
    if (tok == T_LP) {
        lex_next();
        value_t v = eval_expr();
        expect(T_RP);
        return v;
    }
    err("Expected a value or expression");
    return v_num(0);
}

// Exponentiation, tighter than unary minus (so -2^2 = -4), left-associative.
// The exponent is a sign-prefixed primary, so 2^-3 and 2^3^2 (=(2^3)^2) work.
static value_t eval_power(void) {
    value_t a = eval_primary();
    while (!g_err && tok == T_CARET) {
        lex_next();
        int neg = 0;
        while (tok == T_MINUS || tok == T_PLUS) { if (tok == T_MINUS) neg = !neg; lex_next(); }
        value_t b = eval_primary();
        if (g_err) return a;
        if (a.is_str || b.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        a = v_num(dpow(a.num, neg ? -b.num : b.num));
    }
    return a;
}

static value_t eval_unary(void) {
    if (tok == T_MINUS) { lex_next(); value_t v = eval_unary();
                          if (v.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
                          return v_num(-v.num); }
    if (tok == T_PLUS)  { lex_next(); return eval_unary(); }
    if (tok == T_KW && tok_kw == KW_NOT) {        // bitwise complement (NOT TRUE = 0)
        lex_next();
        value_t v = eval_unary();
        if (v.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        return v_num((double)(~(int)v.num));
    }
    return eval_power();
}

static value_t eval_term(void) {
    value_t a = eval_unary();
    while (!g_err && (tok == T_STAR || tok == T_SLASH ||
                      (tok == T_KW && (tok_kw == KW_DIV || tok_kw == KW_MOD)))) {
        int op = tok, kw = tok_kw; lex_next();
        value_t b = eval_unary();
        if (g_err) return a;
        if (a.is_str || b.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        if (op == T_STAR) {
            a = v_num(a.num * b.num);
        } else if (op == T_SLASH) {
            if (b.num == 0) { err("Division by zero"); return v_num(0); }
            a = v_num(a.num / b.num);
        } else {                                  // DIV / MOD: integer, truncate to zero
            long bi = (long)b.num;
            if (bi == 0) { err("Division by zero"); return v_num(0); }
            long ai = (long)a.num;
            a = v_num((double)(kw == KW_DIV ? ai / bi : ai % bi));
        }
    }
    return a;
}

static value_t eval_add(void) {
    value_t a = eval_term();
    while (!g_err && (tok == T_PLUS || tok == T_MINUS)) {
        int op = tok; lex_next();
        value_t b = eval_term();
        if (g_err) return a;
        if (op == T_PLUS && a.is_str && b.is_str) {        // string concatenation
            int total = a.len + b.len;
            if (total > MAX_STR) { err("Text string is too long"); return v_num(0); }
            char *p = scratch_alloc(total);
            if (!p) return v_num(0);
            for (int i = 0; i < a.len; i++) p[i] = a.str[i];
            for (int i = 0; i < b.len; i++) p[a.len + i] = b.str[i];
            a = v_str(p, total);
        } else if (a.is_str || b.is_str) {
            err("Type mismatch: numbers and text can't be mixed"); return v_num(0);
        } else {
            a = v_num(op == T_PLUS ? a.num + b.num : a.num - b.num);
        }
    }
    return a;
}

static int str_cmp(value_t a, value_t b) {
    int n = a.len < b.len ? a.len : b.len;
    for (int i = 0; i < n; i++) {
        unsigned char ca = a.str[i], cb = b.str[i];
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.len == b.len) return 0;
    return a.len < b.len ? -1 : 1;
}

// Relational comparison. BBC BASIC TRUE is -1 (all bits set), FALSE is 0.
static value_t eval_compare(void) {
    value_t a = eval_add();
    if (tok == T_EQ || tok == T_NE || tok == T_LT ||
        tok == T_GT || tok == T_LE || tok == T_GE) {
        int op = tok; lex_next();
        value_t b = eval_add();
        if (g_err) return a;
        long c;
        if (a.is_str && b.is_str)      c = str_cmp(a, b);
        else if (!a.is_str && !b.is_str) c = (a.num > b.num) - (a.num < b.num);
        else { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        int r = 0;
        switch (op) {
            case T_EQ: r = (c == 0); break;
            case T_NE: r = (c != 0); break;
            case T_LT: r = (c <  0); break;
            case T_GT: r = (c >  0); break;
            case T_LE: r = (c <= 0); break;
            case T_GE: r = (c >= 0); break;
        }
        a = v_num(r ? -1.0 : 0.0);
    }
    return a;
}

static value_t eval_and(void) {                   // AND: lower than relational
    value_t a = eval_compare();
    while (!g_err && tok == T_KW && tok_kw == KW_AND) {
        lex_next();
        value_t b = eval_compare();
        if (g_err) return a;
        if (a.is_str || b.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        a = v_num((double)((int)a.num & (int)b.num));
    }
    return a;
}

static value_t eval_expr(void) {                  // OR / EOR: lowest precedence
    value_t a = eval_and();
    while (!g_err && tok == T_KW && (tok_kw == KW_OR || tok_kw == KW_EOR)) {
        int kw = tok_kw; lex_next();
        value_t b = eval_and();
        if (g_err) return a;
        if (a.is_str || b.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        a = v_num((double)(kw == KW_OR ? ((int)a.num | (int)b.num)
                                       : ((int)a.num ^ (int)b.num)));
    }
    return a;
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

// Execution position + control flow.
//
// A "position" is a (program-line index, byte offset into that line's text)
// pair, so GOSUB/RETURN and FOR/NEXT can resume in the *middle* of a line.
static const char *cur_text;     // text of the line currently executing
static int  cur_line_idx;        // prog[] index of that line, or -1 if immediate

static int  g_stop;              // END encountered -> stop RUN
static int  g_branch;            // a jump was requested this statement
static int  g_branch_line;       // target prog[] index
static int  g_branch_off;        // target byte offset within that line

// GOSUB return stack
#define GOSUB_MAX 32
typedef struct { int line; int off; } retaddr_t;
static retaddr_t gosub_stack[GOSUB_MAX];
static int       gosub_sp;

// FOR loop stack
#define FOR_MAX 16
typedef struct {
    char   name[NAME_LEN];
    double limit;
    double step;
    int    line;        // position of the loop body (first thing after FOR...)
    int    off;
} for_rec_t;
static for_rec_t for_stack[FOR_MAX];
static int       for_sp;

// REPEAT loop stack (just the position of the loop body start)
#define REPEAT_MAX 16
static retaddr_t repeat_stack[REPEAT_MAX];
static int       repeat_sp;

// WHILE loop stack: position of the WHILE keyword, so ENDWHILE re-tests it.
#define WHILE_MAX 16
static retaddr_t while_stack[WHILE_MAX];
static int       while_sp;

// CASE nesting depth (the selector is matched immediately, so only the depth
// needs tracking for ENDCASE balancing and nested-CASE skipping).
static int       case_sp;

// Procedures / functions
static int     g_return;         // ENDPROC or =<expr> ends the current PROC/FN body
static value_t fn_retval;        // value returned from an FN via =<expr>
static int     call_sp;          // PROC/FN recursion depth

#define DEF_MAX 64
typedef struct { char name[NAME_LEN]; int is_fn; int line; int off; } defrec_t;
static defrec_t defs[DEF_MAX];
static int      def_n;

static int cur_off(void) { return (int)(lx - cur_text); }

static void branch_to_line(int num) {
    int idx = find_line_index(num);
    if (idx < 0) { err("No such line number"); return; }
    g_branch = 1; g_branch_line = idx; g_branch_off = 0;
}

// Find the program line that begins with the label ".name". Labels are matched
// only as the first token of a line. Returns the prog[] index, or -1.
static int find_label(const char *name) {
    const char *save_text = cur_text;
    const char *save_lx   = lx;
    int save_tok = tok, save_kw = tok_kw;
    int found = -1;
    for (int i = 0; i < prog_n; i++) {
        cur_text = prog[i].text; lx = prog[i].text; lex_next();
        if (tok == T_LABEL && s_eq(tok_var, name)) { found = i; break; }
    }
    cur_text = save_text; lx = save_lx; tok = save_tok; tok_kw = save_kw;
    return found;
}

// Branch to a label by name (GOTO/GOSUB target).
static void branch_to_label(const char *name) {
    int idx = find_label(name);
    if (idx < 0) { err("No such label"); return; }
    g_branch = 1; g_branch_line = idx; g_branch_off = 0;
}

// Reposition the global lexer at (program line index pc, byte offset off) and
// read the first token there. Used by the structured-block forward scanners.
static void lex_at(int pc, int off) {
    cur_text = prog[pc].text;
    lx = cur_text + off;
    lex_next();
}

// Two string values are equal iff same length and same bytes.
static int val_equal(value_t a, value_t b) {
    if (a.is_str != b.is_str) return 0;
    if (a.is_str) {
        if (a.len != b.len) return 0;
        for (int i = 0; i < a.len; i++) if (a.str[i] != b.str[i]) return 0;
        return 1;
    }
    return a.num == b.num;
}

// Forward-scan from the current lexer position for the keyword `close_kw` that
// matches an enclosing `open_kw`, honouring nesting. On success sets g_branch to
// the position just after the matched close keyword and returns 1; on running
// off the end of the program raises `msg` and returns 0. Used by WHILE/ENDWHILE.
static int skip_to_close(int open_kw, int close_kw, const char *msg) {
    int pc = cur_line_idx;
    int depth = 0;
    for (;;) {
        while (tok != T_EOL) {
            if (tok == T_KW && tok_kw == open_kw) depth++;
            else if (tok == T_KW && tok_kw == close_kw) {
                if (depth == 0) { g_branch = 1; g_branch_line = pc; g_branch_off = cur_off(); return 1; }
                depth--;
            }
            lex_next();
        }
        if (++pc >= prog_n) { err(msg); return 0; }
        lex_at(pc, 0);
    }
}

static void exec_statement(void);   // forward decl (IF runs a sub-statement)

#define PRINT_FIELD 8     // column width for the ',' separator and TAB alignment

// PRINT items separated by ; (close up), , (next field), ' (newline), TAB(n),
// SPC(n). Column is tracked from the start of this PRINT (assumed column 0).
static void stmt_print(void) {
    lex_next();                              // consume PRINT
    int col = 0;
    int trailing_sep = 0;                    // line ended with a separator -> no newline
    while (tok != T_EOL && tok != T_COLON && !(tok == T_KW && tok_kw == KW_ELSE)) {
        if (tok == T_SEMI) { lex_next(); trailing_sep = 1; continue; }
        if (tok == T_SQUOTE) { con_putc('\n'); col = 0; lex_next(); trailing_sep = 1; continue; }
        if (tok == T_COMMA) {                // advance to the next field boundary
            do { con_putc(' '); col++; } while (col % PRINT_FIELD);
            lex_next(); trailing_sep = 1; continue;
        }
        if (tok == T_KW && tok_kw == KW_TAB) {
            lex_next(); if (!expect(T_LP)) return;
            int n = (int)need_num(); if (g_err) return;
            if (!expect(T_RP)) return;
            while (col < n) { con_putc(' '); col++; }
            trailing_sep = 1; continue;
        }
        if (tok == T_KW && tok_kw == KW_SPC) {
            lex_next(); if (!expect(T_LP)) return;
            int n = (int)need_num(); if (g_err) return;
            if (!expect(T_RP)) return;
            for (int i = 0; i < n; i++) { con_putc(' '); col++; }
            trailing_sep = 1; continue;
        }
        value_t v = eval_expr();
        if (g_err) return;
        if (v.is_str) { con_putsn(v.str, v.len); col += v.len; }
        else { char b[40]; int n = dbl_to_str(b, v.num); con_putsn(b, n); col += n; }
        trailing_sep = 0;
    }
    if (!trailing_sep) con_putc('\n');
}

static void stmt_let(int had_let) {
    if (had_let) lex_next();                 // consume LET
    if (tok != T_VAR) { err("Expected a variable name"); return; }
    char name[NAME_LEN];
    s_copy(name, tok_var, NAME_LEN);
    int isstr = name_is_str(name);
    lex_next();

    if (tok == T_LP) {                       // array element assignment
        int idx; arr_t *a;
        if (!arr_elem(name, isstr, &idx, &a)) return;
        if (tok != T_EQ) { err("Expected '='"); return; }
        lex_next();
        value_t v = eval_expr();
        if (g_err) return;
        if (a->is_str) {
            if (!v.is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
            str_store_to(&arr_strs[idx], v.str, v.len);
        } else {
            if (v.is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
            arr_nums[idx] = trunc_int(a->is_int, v.num);
        }
        return;
    }

    if (tok != T_EQ) { err("Expected '='"); return; }
    lex_next();
    value_t v = eval_expr();
    if (g_err) return;
    var_t *var = var_find(name);
    if (!var) return;
    if (var->is_str) {
        if (!v.is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
        str_store(var, v.str, v.len);
    } else {
        if (v.is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
        var->num = trunc_int(var->is_int, v.num);
    }
}

static void stmt_goto(void) {
    lex_next();
    // A label target may be written bare (GOTO start) or dotted (GOTO .start).
    if (tok == T_LABEL || tok == T_VAR) { char nm[NAME_LEN]; s_copy(nm, tok_var, NAME_LEN);
                          lex_next(); branch_to_label(nm); return; }
    if (tok != T_NUM) { err("Expected a line number or label"); return; }
    int target = (int)tok_num;
    lex_next();
    branch_to_line(target);
}

// Run ':'-separated statements from the current position until ELSE / EOL or a
// control transfer. Used for the THEN and ELSE clauses of IF.
static void exec_clause(void) {
    while (!g_err && !g_stop && !g_branch && tok != T_EOL &&
           !(tok == T_KW && tok_kw == KW_ELSE)) {
        exec_statement();
        if (g_err || g_stop || g_branch) return;
        if (tok == T_COLON) { lex_next(); continue; }
        break;
    }
}

// Consume the rest of the current IF line, returning 1 if THEN was the last
// token on it (the block-IF form `IF cond THEN` <newline>). The lexer is left
// at EOL. Used while scanning so a nested single-line IF (whose ELSE belongs to
// itself) is skipped as a whole and never miscounted.
static int scan_if_line_is_block(void) {
    int last_then = 0;
    lex_next();                              // consume IF
    while (tok != T_EOL) {
        last_then = (tok == T_KW && tok_kw == KW_THEN);
        lex_next();
    }
    return last_then;
}

// Forward-scan for the ELSE/ENDIF that matches the current block IF. Nested
// block IFs increment the depth; single-line IFs are skipped whole. With
// else_too set we stop at the first depth-0 ELSE *or* ENDIF (the false branch
// jumps to whichever comes first); otherwise only ENDIF (a finished THEN branch
// jumps past its ELSE block). Branches past the matched keyword. Returns 1.
static int skip_if_block(int else_too) {
    int pc = cur_line_idx;
    int depth = 0;
    for (;;) {
        while (tok != T_EOL) {
            if (tok == T_KW && tok_kw == KW_IF) {
                if (scan_if_line_is_block()) depth++;   // consumes to EOL
                break;
            }
            if (tok == T_KW && tok_kw == KW_ENDIF) {
                if (depth == 0) { g_branch = 1; g_branch_line = pc; g_branch_off = cur_off(); return 1; }
                depth--;
            } else if (else_too && depth == 0 && tok == T_KW && tok_kw == KW_ELSE) {
                g_branch = 1; g_branch_line = pc; g_branch_off = cur_off(); return 1;
            }
            lex_next();
        }
        if (++pc >= prog_n) { err("IF without a matching ENDIF"); return 0; }
        lex_at(pc, 0);
    }
}

// IF <expr> [THEN] (<line>|stmts) [ELSE (<line>|stmts)]    -- single-line form
// IF <expr> THEN <newline> ... [ELSE ...] ENDIF            -- block form
// THEN is optional in the single-line form; the block form is recognised when
// THEN is the last token on the line.
static void stmt_if(void) {
    lex_next();                              // consume IF
    value_t cv = eval_expr();
    if (g_err) return;
    long cond = cv.is_str ? (cv.len != 0) : (cv.num != 0);
    int had_then = 0;
    if (tok == T_KW && tok_kw == KW_THEN) { had_then = 1; lex_next(); }

    if (had_then && tok == T_EOL) {          // block IF (THEN ends the line)
        if (cur_line_idx < 0) { err("Block IF can only be used inside a program"); return; }
        if (cond) return;                    // fall through: run the THEN block
        skip_if_block(1);                    // false: jump to ELSE or ENDIF
        return;
    }

    if (cond) {                              // single-line IF
        if (tok == T_NUM) { int t = (int)tok_num; lex_next(); branch_to_line(t); }
        else exec_clause();                  // run THEN clause (stops at ELSE/EOL)
    } else {
        while (tok != T_EOL && !(tok == T_KW && tok_kw == KW_ELSE)) lex_next();  // skip THEN
        if (tok == T_KW && tok_kw == KW_ELSE) {
            lex_next();
            if (tok == T_NUM) { int t = (int)tok_num; lex_next(); branch_to_line(t); }
            else exec_clause();              // run ELSE clause
        }
    }
    if (!g_branch && !g_err && !g_stop) tok = T_EOL;   // discard any leftover ELSE part
}

// A standalone ELSE statement is only reached after running a block IF's THEN
// branch: skip past the matching ENDIF.
static void stmt_else_block(void) {
    lex_next();                              // consume ELSE
    if (cur_line_idx < 0) { err("ELSE without a matching IF"); return; }
    skip_if_block(0);
}

static void stmt_repeat(void) {
    lex_next();                              // consume REPEAT
    if (cur_line_idx < 0) { err("This can only be used inside a program"); return; }
    if (repeat_sp >= REPEAT_MAX) { err("Too many nested REPEAT loops"); return; }
    repeat_stack[repeat_sp].line = cur_line_idx;   // loop body starts here
    repeat_stack[repeat_sp].off  = cur_off();
    repeat_sp++;
}

static void stmt_until(void) {
    lex_next();                              // consume UNTIL
    double cond = need_num();
    if (g_err) return;
    if (repeat_sp <= 0) { err("UNTIL without a matching REPEAT"); return; }
    if (cond == 0) {                         // condition false -> loop again
        retaddr_t *r = &repeat_stack[repeat_sp - 1];
        g_branch = 1; g_branch_line = r->line; g_branch_off = r->off;
    } else {
        repeat_sp--;                         // true -> exit the loop
    }
}

// WHILE <expr> ... ENDWHILE : a pre-tested loop. The condition is re-evaluated
// each pass, so ENDWHILE branches back to the WHILE keyword (re-running this).
// To keep the stack balanced, ENDWHILE pops before branching back and each
// WHILE pass pushes exactly once.
static void stmt_while(void) {
    int kw_line = cur_line_idx;
    int kw_off  = (int)(tok_start - cur_text);   // offset of the WHILE keyword
    lex_next();                                  // consume WHILE
    value_t cv = eval_expr();
    if (g_err) return;
    long cond = cv.is_str ? (cv.len != 0) : (cv.num != 0);
    if (cond) {
        if (cur_line_idx < 0) { err("WHILE can only be used inside a program"); return; }
        if (while_sp >= WHILE_MAX) { err("Too many nested WHILE loops"); return; }
        while_stack[while_sp].line = kw_line;
        while_stack[while_sp].off  = kw_off;
        while_sp++;                              // run the loop body
    } else {
        skip_to_close(KW_WHILE, KW_ENDWHILE, "WHILE without a matching ENDWHILE");
    }
}

static void stmt_endwhile(void) {
    if (while_sp <= 0) { err("ENDWHILE without a matching WHILE"); return; }
    while_sp--;
    retaddr_t r = while_stack[while_sp];
    g_branch = 1; g_branch_line = r.line; g_branch_off = r.off;  // back to WHILE
}

// CASE <expr> OF / WHEN <e>[,<e>...] / OTHERWISE / ENDCASE : multi-way select.
// The selector is matched immediately by scanning the WHEN clauses; control then
// jumps into the matching clause (or OTHERWISE, or past ENDCASE if none match).
// Falling through into a later WHEN/OTHERWISE means the chosen clause finished,
// so those jump to ENDCASE.
static void stmt_case(void) {
    lex_next();                                  // consume CASE
    value_t sel = eval_expr();
    if (g_err) return;
    // Copy a string selector somewhere stable: evaluating WHEN expressions reuses
    // scratch and may trigger a GC that relocates heap strings.
    static char selbuf[MAX_STR];
    if (sel.is_str) {
        if (sel.len > MAX_STR) { err("Text string is too long"); return; }
        for (int i = 0; i < sel.len; i++) selbuf[i] = sel.str[i];
        sel.str = selbuf;
    }
    if (tok != T_KW || tok_kw != KW_OF) { err("Expected OF after CASE"); return; }
    if (cur_line_idx < 0) { err("CASE can only be used inside a program"); return; }
    if (case_sp >= 16) { err("Too many nested CASE statements"); return; }
    case_sp++;

    // Scan forward for the matching WHEN/OTHERWISE/ENDCASE.
    int pc = cur_line_idx;
    int depth = 0;
    for (;;) {
        while (tok != T_EOL) {
            if (tok == T_KW && tok_kw == KW_CASE) depth++;
            else if (tok == T_KW && tok_kw == KW_ENDCASE) {
                if (depth == 0) {                // no clause matched, no OTHERWISE
                    case_sp--;
                    g_branch = 1; g_branch_line = pc; g_branch_off = cur_off();
                    return;
                }
                depth--;
            } else if (depth == 0 && tok == T_KW && tok_kw == KW_OTHERWISE) {
                g_branch = 1; g_branch_line = pc; g_branch_off = cur_off();  // run OTHERWISE body
                return;
            } else if (depth == 0 && tok == T_KW && tok_kw == KW_WHEN) {
                lex_next();                      // evaluate this clause's value list
                int matched = 0;
                for (;;) {
                    value_t v = eval_expr();
                    if (g_err) return;
                    if (val_equal(sel, v)) matched = 1;
                    if (tok == T_COMMA) { lex_next(); continue; }
                    break;
                }
                if (matched) {                   // run this clause's body
                    g_branch = 1; g_branch_line = pc; g_branch_off = cur_off();
                    return;
                }
                continue;                        // no match: keep scanning this line
            }
            lex_next();
        }
        if (++pc >= prog_n) { err("CASE without a matching ENDCASE"); case_sp--; return; }
        lex_at(pc, 0);
    }
}

// Reached by falling through after a chosen clause's body finished: jump past
// the matching ENDCASE. (Encountered as a statement, not during the CASE scan.)
static void case_skip_to_end(void) {
    if (case_sp <= 0) { err("WHEN/OTHERWISE without a matching CASE"); return; }
    if (skip_to_close(KW_CASE, KW_ENDCASE, "CASE without a matching ENDCASE"))
        case_sp--;
}

static void stmt_endcase(void) {
    lex_next();                                  // consume ENDCASE
    if (case_sp > 0) case_sp--;
}

static void stmt_colour(void) {
    lex_next();                              // consume COLOUR
    int c = (int)need_num();
    if (g_err) return;
    con_colour(c);                           // BBC COLOUR n (foreground 0..7)
}

// LOCAL var[,var...] : save the named variables' current values (restored when
// the enclosing PROC/FN ends) and reset them to zero/empty.
static void stmt_local(void) {
    lex_next();                              // consume LOCAL
    for (;;) {
        if (tok != T_VAR) { err("Expected a variable name"); return; }
        if (local_sp >= LOCAL_MAX) { err("Out of memory"); return; }
        var_t *v = var_find(tok_var);
        if (!v) return;
        local_stack[local_sp].slot = v;
        local_stack[local_sp].old  = *v;     // save (descriptor stays a GC root)
        local_sp++;
        v->num = 0; v->s.sptr = 0; v->s.slen = 0;   // reset for the procedure
        lex_next();
        if (tok == T_COMMA) { lex_next(); continue; }
        break;
    }
}

static void stmt_gosub(void) {
    lex_next();
    char label[NAME_LEN]; int is_label = 0; int target = 0;
    if (tok == T_LABEL || tok == T_VAR) { s_copy(label, tok_var, NAME_LEN); is_label = 1; lex_next(); }
    else if (tok == T_NUM) { target = (int)tok_num; lex_next(); }
    else { err("Expected a line number or label"); return; }
    if (cur_line_idx < 0) { err("This can only be used inside a program"); return; }
    if (gosub_sp >= GOSUB_MAX) { err("Out of memory"); return; }
    gosub_stack[gosub_sp].line = cur_line_idx;   // resume after the GOSUB
    gosub_stack[gosub_sp].off  = cur_off();
    gosub_sp++;
    if (is_label) branch_to_label(label); else branch_to_line(target);
    if (g_err) gosub_sp--;                        // undefined target: undo push
}

static void stmt_return(void) {
    lex_next();
    if (gosub_sp <= 0) { err("RETURN without a matching GOSUB"); return; }
    gosub_sp--;
    g_branch = 1;
    g_branch_line = gosub_stack[gosub_sp].line;
    g_branch_off  = gosub_stack[gosub_sp].off;
}

static void stmt_for(void) {
    lex_next();                              // consume FOR
    if (tok != T_VAR) { err("Expected a variable name"); return; }
    char name[NAME_LEN];
    s_copy(name, tok_var, NAME_LEN);
    lex_next();
    if (tok != T_EQ) { err("Expected '='"); return; }
    lex_next();
    double start = need_num(); if (g_err) return;
    if (tok != T_KW || tok_kw != KW_TO) { err("Expected TO in a FOR statement"); return; }
    lex_next();
    double limit = need_num(); if (g_err) return;
    double step = 1;
    if (tok == T_KW && tok_kw == KW_STEP) {
        lex_next();
        step = need_num(); if (g_err) return;
    }
    var_t *r = var_find(name); if (!r) return;
    if (r->is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
    r->num = trunc_int(r->is_int, start);

    if (cur_line_idx < 0) { err("This can only be used inside a program"); return; }
    if (for_sp >= FOR_MAX) { err("Out of memory"); return; }
    s_copy(for_stack[for_sp].name, name, NAME_LEN);
    for_stack[for_sp].limit = limit;
    for_stack[for_sp].step  = step;
    for_stack[for_sp].line  = cur_line_idx;   // loop body starts right here
    for_stack[for_sp].off   = cur_off();
    for_sp++;
    // Test happens at NEXT, so the body always runs at least once (MS BASIC).
}

static void stmt_next(void) {
    lex_next();                              // consume NEXT
    char name[NAME_LEN]; name[0] = 0;
    if (tok == T_VAR) { s_copy(name, tok_var, NAME_LEN); lex_next(); }

    if (for_sp <= 0) { err("NEXT without a matching FOR"); return; }
    int idx = for_sp - 1;
    if (name[0]) {                           // NEXT I: pop inner loops to reach I
        while (idx >= 0 && !s_eq(for_stack[idx].name, name)) idx--;
        if (idx < 0) { err("NEXT without a matching FOR"); return; }
        for_sp = idx + 1;
    }

    for_rec_t *f = &for_stack[idx];
    var_t *r = var_find(f->name); if (!r) return;
    r->num = trunc_int(r->is_int, r->num + f->step);
    int cont = (f->step >= 0) ? (r->num <= f->limit) : (r->num >= f->limit);
    if (cont) {
        g_branch = 1; g_branch_line = f->line; g_branch_off = f->off;
    } else {
        for_sp = idx;                        // loop done: pop it
    }
}

static void stmt_dim(void) {
    lex_next();                              // consume DIM
    for (;;) {
        if (tok != T_VAR) { err("Expected a variable name"); return; }
        char name[NAME_LEN];
        s_copy(name, tok_var, NAME_LEN);
        int isstr = name_is_str(name);
        lex_next();
        if (tok != T_LP) { err("Expected '(' after the array name"); return; }

        int subs[MAX_DIMS];
        int nsub;
        if (!parse_subscripts(subs, &nsub)) return;
        if (arr_find(name)) { err("That array is already defined"); return; }
        int counts[MAX_DIMS];
        for (int i = 0; i < nsub; i++) {
            if (subs[i] < 0) { err("Array index out of range"); return; }
            counts[i] = subs[i] + 1;         // DIM A(10) -> indices 0..10
        }
        if (!arr_create(name, nsub, counts, isstr)) return;

        if (tok == T_COMMA) { lex_next(); continue; }
        break;
    }
}

static void stmt_input(void) {
    lex_next();                              // consume INPUT
    if (tok == T_STR) {                      // optional prompt:  INPUT "NAME"; A$
        int n = 0; while (tok_str[n]) n++;
        con_putsn(tok_str, n);
        lex_next();
        if (tok == T_SEMI || tok == T_COMMA) lex_next();
    }
    con_puts("? ");

    static char inbuf[LINE_LEN];
    int got = con_getline(inbuf, LINE_LEN);
    if (got < 0) { g_stop = 1; return; }     // host EOF ends the program
    int ip = 0;

    for (;;) {
        if (tok != T_VAR) { err("Expected a variable name"); return; }
        char name[NAME_LEN];
        s_copy(name, tok_var, NAME_LEN);
        lex_next();
        var_t *var = var_find(name);
        if (!var) return;

        while (inbuf[ip] == ' ') ip++;       // one comma-delimited field
        int start = ip;
        while (inbuf[ip] && inbuf[ip] != ',') ip++;
        int end = ip;

        if (var->is_str) {
            str_store(var, inbuf + start, end - start);
        } else {
            var->num = parse_double(inbuf + start, end - start, 0);
        }
        if (g_err) return;

        if (inbuf[ip] == ',') ip++;
        if (tok == T_COMMA) { lex_next(); continue; }   // more variables to fill
        break;
    }
}

static void run_program(int start_pc, int start_off);

// Print a program line for LIST with the classic look: keywords are shown in
// UPPERCASE, while variable names, numbers, string literals and REM comments are
// left exactly as the user typed them. This is display-only; the stored text and
// the way the line runs are unchanged.
static void list_text(const char *t) {
    int i = 0;
    while (t[i]) {
        char c = t[i];
        if (c == '"') {                          // string literal: copy verbatim
            con_putc(c); i++;
            while (t[i] && t[i] != '"') con_putc(t[i++]);
            if (t[i] == '"') { con_putc('"'); i++; }
            continue;
        }
        if (!is_alpha(c)) { con_putc(c); i++; continue; }   // digits/punctuation/space

        // Gather a word (letters+digits, optional $ or % suffix) and a folded
        // copy to test against the keyword table.
        int j = i, wn = 0; char word[16];
        while (is_alnum(t[j])) { if (wn < 15) word[wn++] = up(t[j]); j++; }
        if ((t[j] == '$' || t[j] == '%') && wn < 15) word[wn++] = t[j], j++;
        word[wn] = 0;

        int is_kw = 0;
        for (int k = 0; k < kwcount; k++)
            if (s_eq(word, kwtab[k].name)) { is_kw = 1; break; }
        int is_proc = (word[0]=='P'&&word[1]=='R'&&word[2]=='O'&&word[3]=='C');
        int is_fn   = (word[0]=='F'&&word[1]=='N'&&wn>2);

        if (is_kw) {
            for (int p = i; p < j; p++) con_putc(up(t[p]));    // keyword -> UPPERCASE
            if (s_eq(word, "REM")) { i = j; while (t[i]) con_putc(t[i++]); break; }
        } else if (is_proc || is_fn) {                         // PROC/FN + name
            int plen = is_proc ? 4 : 2;
            for (int p = i; p < i + plen; p++) con_putc(up(t[p]));
            for (int p = i + plen; p < j; p++) con_putc(t[p]); // name as typed
        } else {
            for (int p = i; p < j; p++) con_putc(t[p]);        // variable as typed
        }
        i = j;
    }
}

// LIST [start][,end] : whole program, a single line (LIST 100), a range
// (LIST 100,200), from a line (LIST 100,) or up to a line (LIST ,200).
static void stmt_list(void) {
    lex_next();
    int lo = 0, hi = 0x7FFFFFFF;
    if (tok == T_NUM) { lo = (int)tok_num; hi = lo; lex_next(); }   // single line by default
    if (tok == T_COMMA) {                                           // a range was requested
        lex_next();
        hi = 0x7FFFFFFF;
        if (tok == T_NUM) { hi = (int)tok_num; lex_next(); }
    }
    for (int i = 0; i < prog_n; i++) {
        if (prog[i].num < lo || prog[i].num > hi) continue;
        con_putn(prog[i].num);
        con_putc(' ');
        list_text(prog[i].text);
        con_putc('\n');
    }
}

// ---------------------------------------------------------------------------
// DATA / READ / RESTORE. The data pointer walks the program's DATA statements
// using plain text scanning (no lexer state), so READ can run mid-statement.
// ---------------------------------------------------------------------------

static int  data_pc  = 0;        // prog index to scan for DATA from
static int  data_off = -1;       // offset of next item in prog[data_pc], -1 = not located
static char data_item[LINE_LEN];

static void data_reset(int line_num) {
    if (line_num < 0) data_pc = 0;
    else { int idx = find_line_index(line_num); data_pc = (idx < 0) ? prog_n : idx; }
    data_off = -1;
}

static int line_is_data(const char *t, int *body) {
    int i = 0; while (t[i] == ' ' || t[i] == '\t') i++;
    if (up(t[i]) == 'D' && up(t[i+1]) == 'A' && up(t[i+2]) == 'T' &&
        up(t[i+3]) == 'A' && !is_alnum(t[i+4])) { *body = i + 4; return 1; }
    return 0;
}

// Fetch the next DATA item into data_item[]; returns 1, or 0 when out of data.
static int data_next(int *len) {
    for (;;) {
        if (data_off < 0) {                      // locate the next DATA line
            int body;
            while (data_pc < prog_n && !line_is_data(prog[data_pc].text, &body)) data_pc++;
            if (data_pc >= prog_n) return 0;
            data_off = body;
        }
        const char *t = prog[data_pc].text;
        int i = data_off;
        while (t[i] == ' ' || t[i] == '\t') i++;
        if (t[i] == 0) { data_pc++; data_off = -1; continue; }   // end of this DATA line

        int n = 0;
        if (t[i] == '"') {                       // quoted string item
            i++;
            while (t[i] && t[i] != '"') { if (n < LINE_LEN - 1) data_item[n++] = t[i]; i++; }
            if (t[i] == '"') i++;
        } else {                                 // unquoted: up to ',' (trim trailing space)
            int s = i;
            while (t[i] && t[i] != ',') i++;
            int e = i; while (e > s && (t[e-1] == ' ' || t[e-1] == '\t')) e--;
            for (int k = s; k < e && n < LINE_LEN - 1; k++) data_item[n++] = t[k];
        }
        while (t[i] == ' ' || t[i] == '\t') i++;
        if (t[i] == ',') i++;
        data_off = i;
        data_item[n] = 0;
        *len = n;
        return 1;
    }
}

static void stmt_on(void) {
    lex_next();                                  // consume ON
    int sel = (int)need_num();
    if (g_err) return;
    if (tok != T_KW || (tok_kw != KW_GOTO && tok_kw != KW_GOSUB)) { err("Expected GOTO or GOSUB after ON"); return; }
    int is_gosub = (tok_kw == KW_GOSUB);
    lex_next();
    int target = -1, i = 1;
    for (;;) {                                   // pick the sel-th line number (1-based)
        if (tok != T_NUM) { err("Expected a line number"); return; }
        if (i == sel) target = (int)tok_num;
        lex_next();
        i++;
        if (tok == T_COMMA) { lex_next(); continue; }
        break;
    }
    if (target < 0) return;                      // out of range: fall through to next line
    if (is_gosub) {
        if (cur_line_idx < 0) { err("This can only be used inside a program"); return; }
        if (gosub_sp >= GOSUB_MAX) { err("Out of memory"); return; }
        gosub_stack[gosub_sp].line = cur_line_idx;
        gosub_stack[gosub_sp].off  = cur_off();
        gosub_sp++;
    }
    branch_to_line(target);
}

// VDU n[,n...][;] : send each value's bytes to the VDU driver. A value followed
// by ';' is sent as a 16-bit word (two bytes, least-significant first); otherwise
// just its least-significant byte is sent. A trailing '|' (BBC shorthand) sends
// nine zero bytes, padding out a VDU 23 command.
static void stmt_vdu(void) {
    lex_next();                                  // consume VDU
    if (tok == T_EOL || tok == T_COLON) return;  // bare VDU does nothing
    for (;;) {
        int v = (int)need_num();
        if (g_err) return;
        if (tok == T_SEMI) {                     // 16-bit: low byte then high byte
            con_vdu(v & 0xff);
            con_vdu((v >> 8) & 0xff);
            lex_next();
        } else {                                 // 8-bit: low byte only
            con_vdu(v & 0xff);
            if (tok == T_COMMA) lex_next();
            else break;
        }
        if (tok == T_EOL || tok == T_COLON) break;
    }
}

static void stmt_restore(void) {
    lex_next();                                  // consume RESTORE
    int line = -1;
    if (tok == T_NUM) { line = (int)tok_num; lex_next(); }
    data_reset(line);
}

// READ var[,var...] : assign successive DATA items (numeric or string).
static void stmt_read(void) {
    lex_next();                                  // consume READ
    for (;;) {
        if (tok != T_VAR) { err("Expected a variable name"); return; }
        char name[NAME_LEN]; s_copy(name, tok_var, NAME_LEN);
        int isstr = name_is_str(name);
        lex_next();
        int is_arr = 0, idx = 0; arr_t *a = 0; var_t *var = 0;
        if (tok == T_LP) {
            if (!arr_elem(name, isstr, &idx, &a)) return;
            is_arr = 1;
        } else {
            var = var_find(name); if (!var) return;
        }
        int len;
        if (!data_next(&len)) { err("READ ran out of DATA"); return; }
        if (isstr) {
            if (is_arr) { if (!a->is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
                          str_store_to(&arr_strs[idx], data_item, len); }
            else        { if (!var->is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
                          str_store(var, data_item, len); }
        } else {
            double dv = parse_double(data_item, len, 0);
            if (is_arr) { if (a->is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
                          arr_nums[idx] = trunc_int(a->is_int, dv); }
            else        { if (var->is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
                          var->num = trunc_int(var->is_int, dv); }
        }
        if (g_err) return;
        if (tok == T_COMMA) { lex_next(); continue; }
        break;
    }
}

// --- Storage statements (LOAD / SAVE / CAT / DELETE) -------------------------
#define STG_BUF_SIZE 65536
static char stg_buf[STG_BUF_SIZE];

// Map a storage error code to a BASIC error message.
static void stg_err(int code) {
    switch (code) {
        case STG_ENOTFOUND: err("File not found");                 break;
        case STG_EFULL:     err("Storage card is full");           break;
        case STG_ENOFS:     err("No storage card found");          break;
        case STG_ETOOBIG:   err("File is too big to load");        break;
        default:            err("Storage read/write error");       break;
    }
}

// Read the next filename operand into a NUL-terminated buffer. Accepts either a
// quoted string ("name.bas") or a bare filename typed without quotes (name.bas
// or just name). Reads straight from the raw source so characters like '.' don't
// trip the expression tokeniser, then resyncs the tokeniser. If no extension is
// given, ".BAS" is appended, so "LOAD WELCOME" finds WELCOME.BAS. The FAT layer
// is case-insensitive, so the name is kept as typed. Callers must NOT lex past
// the keyword first - lx still points just after LOAD/SAVE/DELETE.
static int read_filename(char *out, int outsz) {
    while (is_space(*lx)) lx++;
    int n = 0;
    if (*lx == '"') {                          // quoted form: "name"
        lx++;
        while (*lx && *lx != '"') { if (n < outsz - 1) out[n++] = *lx; lx++; }
        if (*lx == '"') lx++;
    } else {                                   // bare form: letters digits . / _ -
        while (*lx && (is_alnum(*lx) || *lx == '.' || *lx == '/' ||
                       *lx == '_' || *lx == '-')) {
            if (n < outsz - 1) out[n++] = *lx;
            lx++;
        }
    }
    out[n] = 0;
    if (n == 0) { err("Expected a file name"); return 0; }

    // Default the extension to .BAS when the name has none.
    int has_dot = 0;
    for (int i = 0; i < n; i++) if (out[i] == '.') has_dot = 1;
    if (!has_dot && n + 4 < outsz) {
        out[n++] = '.'; out[n++] = 'B'; out[n++] = 'A'; out[n++] = 'S';
        out[n] = 0;
    }

    lex_next();                                // resync the tokeniser for what follows
    return 1;
}

static int uint_to_str(char *p, int v) {        // unsigned decimal -> p, returns length
    char t[12]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) p[i] = t[n - 1 - i];
    return n;
}

// SAVE "name" : serialise the program ("<num> <text>\n" per line) and write it.
static void stmt_save(void) {
    char name[64];
    if (!read_filename(name, sizeof name)) return;
    int pos = 0;
    for (int i = 0; i < prog_n && pos < STG_BUF_SIZE - LINE_LEN - 16; i++) {
        pos += uint_to_str(stg_buf + pos, prog[i].num);
        stg_buf[pos++] = ' ';
        const char *t = prog[i].text;
        while (*t && pos < STG_BUF_SIZE - 2) stg_buf[pos++] = *t++;
        stg_buf[pos++] = '\n';
    }
    int r = stg_write(name, stg_buf, pos);
    if (r) stg_err(r);
}

// LOAD "name" : clear the current program and parse the file's numbered lines.
static void stmt_load(void) {
    char name[64];
    if (!read_filename(name, sizeof name)) return;
    int n = stg_read(name, stg_buf, STG_BUF_SIZE);
    if (n < 0) { stg_err(n); return; }

    prog_n = 0;
    clear_vars();
    int i = 0;
    while (i < n) {
        char line[LINE_LEN];
        int j = 0;
        while (i < n && stg_buf[i] != '\n' && stg_buf[i] != '\r') {
            if (j < LINE_LEN - 1) line[j++] = stg_buf[i];
            i++;
        }
        line[j] = 0;
        while (i < n && (stg_buf[i] == '\n' || stg_buf[i] == '\r')) i++;
        char *p = line;
        while (is_space(*p)) p++;
        if (is_digit(*p)) {
            int num = 0;
            while (is_digit(*p)) { num = num * 10 + (*p - '0'); p++; }
            while (is_space(*p)) p++;
            prog_store(num, p);
        }
    }
}

static void stmt_delete(void) {
    char name[64];
    if (!read_filename(name, sizeof name)) return;
    int r = stg_delete(name);
    if (r) stg_err(r);
}

// --- RENUMBER ---------------------------------------------------------------
static int g_renum_start = 10, g_renum_step = 10;

// Map an old line number to its new one. References to a line that does not
// exist are left unchanged.
static int remap_line(int old) {
    for (int i = 0; i < prog_n; i++)
        if (prog[i].num == old) return g_renum_start + i * g_renum_step;
    return old;
}

// Rewrite one line's text into `out`, remapping the line-number references that
// follow GOTO / GOSUB / RESTORE / THEN / ELSE (including the comma lists of
// ON ... GOTO / ON ... GOSUB). Numeric literals in expressions are left alone.
static void renum_fixup_line(const char *in, char *out) {
    const char *p = in;
    int oi = 0;
    int linref = 0;          // 0 none, 1 GOTO/GOSUB/RESTORE list, 2 THEN/ELSE (optional)
    while (*p && oi < LINE_LEN - 12) {
        char c = *p;
        if (c == '"') {                                   // string literal: copy verbatim
            out[oi++] = c; p++;
            while (*p && *p != '"' && oi < LINE_LEN - 1) out[oi++] = *p++;
            if (*p == '"') out[oi++] = *p++;
            continue;
        }
        if (is_alpha(c)) {                                // a word: keyword or identifier
            char w[16]; int wn = 0;
            while (is_alnum(*p)) { if (wn < 15) w[wn++] = up(*p); out[oi++] = *p; p++; }
            w[wn] = 0;
            if (s_eq(w, "GOTO") || s_eq(w, "GOSUB") || s_eq(w, "RESTORE")) linref = 1;
            else if (s_eq(w, "THEN") || s_eq(w, "ELSE")) linref = 2;
            else linref = 0;
            continue;
        }
        if (is_digit(c) && linref) {                      // a line-number reference
            int num = 0;
            while (is_digit(*p)) { num = num * 10 + (*p - '0'); p++; }
            oi += uint_to_str(out + oi, remap_line(num));
            if (linref == 2) linref = 0;                  // THEN/ELSE take one line ref
            continue;
        }
        if (is_digit(c)) {                                // a plain numeric literal: copy
            while (is_digit(*p) || *p == '.') out[oi++] = *p++;
            continue;
        }
        if (c != ',' && c != ' ' && c != '\t' && linref == 1) linref = 0;  // list ended
        out[oi++] = c; p++;
    }
    out[oi] = 0;
}

// RENUMBER [start][,step] : renumber the program (default 10,10) and fix up all
// line-number references so GOTO/GOSUB/RESTORE/THEN/ELSE/ON still point correctly.
static void stmt_renumber(void) {
    lex_next();                                  // consume RENUMBER
    g_renum_start = 10; g_renum_step = 10;
    if (tok == T_NUM) { g_renum_start = (int)tok_num; lex_next(); }
    if (tok == T_COMMA) { lex_next(); if (tok == T_NUM) { g_renum_step = (int)tok_num; lex_next(); } }
    if (g_renum_start < 0) g_renum_start = 0;
    if (g_renum_step  < 1) g_renum_step  = 1;
    char tmp[LINE_LEN];
    for (int i = 0; i < prog_n; i++) {           // remap references (old numbers still in place)
        renum_fixup_line(prog[i].text, tmp);
        s_copy(prog[i].text, tmp, LINE_LEN);
    }
    for (int i = 0; i < prog_n; i++)             // then renumber the lines
        prog[i].num = g_renum_start + i * g_renum_step;
}

// --- AUTO / EDIT ------------------------------------------------------------
// State the REPL reads to pre-fill the next input line.
static int  g_auto_active = 0;        // AUTO mode: auto-number each entered line
static int  g_auto_num    = 0;        // next line number to offer
static int  g_auto_step   = 10;
static char g_prefill[LINE_LEN];      // one-shot prefill for the next line (EDIT)
static int  g_prefill_len = 0;

// AUTO [start][,step] : enter auto line-numbering. The REPL offers each line
// number for editing; pressing Return on an empty line leaves AUTO.
static void stmt_auto(void) {
    lex_next();
    int start = 10, step = 10;
    if (tok == T_NUM) { start = (int)tok_num; lex_next(); }
    if (tok == T_COMMA) { lex_next(); if (tok == T_NUM) { step = (int)tok_num; lex_next(); } }
    if (start < 0) start = 0;
    if (step  < 1) step  = 1;
    g_auto_num = start; g_auto_step = step; g_auto_active = 1;
}

// EDIT n : recall line n into the input line, ready to edit and re-enter.
static void stmt_edit(void) {
    lex_next();
    if (tok != T_NUM) { err("Expected a line number"); return; }
    int num = (int)tok_num; lex_next();
    int idx = find_line_index(num);
    if (idx < 0) { err("No such line number"); return; }
    int n = uint_to_str(g_prefill, num);
    g_prefill[n++] = ' ';
    const char *t = prog[idx].text;
    while (*t && n < LINE_LEN - 1) g_prefill[n++] = *t++;
    g_prefill[n] = 0;
    g_prefill_len = n;
}

// --- Graphics statements ----------------------------------------------------
static void stmt_mode(void) {
    lex_next();                                  // consume MODE
    int n = (int)need_num();
    if (g_err) return;
    con_mode(n);
}

// GCOL action,colour  (or GCOL colour, meaning action 0)
static void stmt_gcol(void) {
    lex_next();                                  // consume GCOL
    int a = (int)need_num();
    if (g_err) return;
    if (tok == T_COMMA) {
        lex_next();
        int c = (int)need_num();
        if (g_err) return;
        con_gcol(a, c);
    } else {
        con_gcol(0, a);
    }
}

// PLOT code,x,y
static void stmt_plot(void) {
    lex_next();                                  // consume PLOT
    int k = (int)need_num(); if (!expect(T_COMMA)) return;
    int x = (int)need_num(); if (!expect(T_COMMA)) return;
    int y = (int)need_num(); if (g_err) return;
    con_plot(k, x, y);
}

// MOVE x,y  ==  PLOT 4,x,y     DRAW x,y  ==  PLOT 5,x,y
static void stmt_move_draw(int plotcode) {
    lex_next();                                  // consume MOVE/DRAW
    int x = (int)need_num(); if (!expect(T_COMMA)) return;
    int y = (int)need_num(); if (g_err) return;
    con_plot(plotcode, x, y);
}

static void exec_statement(void) {
    scratch_reset();                             // reclaim previous temporaries
    if (tok == T_LABEL) lex_next();              // ".name": a branch label, then run the rest
    if (tok == T_EOL || tok == T_COLON) return;
    if (tok == T_KW) {
        switch (tok_kw) {
            case KW_PRINT: stmt_print();   return;
            case KW_LET:   stmt_let(1);    return;
            case KW_INPUT: stmt_input();   return;
            case KW_DIM:   stmt_dim();     return;
            case KW_GOTO:  stmt_goto();    return;
            case KW_GOSUB: stmt_gosub();   return;
            case KW_RETURN:stmt_return();  return;
            case KW_FOR:   stmt_for();     return;
            case KW_NEXT:  stmt_next();    return;
            case KW_IF:    stmt_if();      return;
            case KW_REPEAT:stmt_repeat();  return;
            case KW_UNTIL: stmt_until();   return;
            case KW_WHILE: stmt_while();   return;
            case KW_ENDWHILE: stmt_endwhile(); return;
            case KW_CASE:  stmt_case();    return;
            case KW_WHEN:
            case KW_OTHERWISE: case_skip_to_end(); return;  // chosen clause finished
            case KW_ENDCASE: stmt_endcase(); return;
            case KW_ENDIF: lex_next();     return;   // block-IF join point: no-op
            case KW_ELSE:  stmt_else_block(); return;   // end of a block-IF THEN branch
            case KW_CLS:   lex_next(); con_cls();   return;
            case KW_COLOUR:stmt_colour();  return;
            case KW_MODE:  stmt_mode();    return;
            case KW_GCOL:  stmt_gcol();    return;
            case KW_PLOT:  stmt_plot();    return;
            case KW_MOVE:  stmt_move_draw(4); return;   // MOVE = PLOT 4
            case KW_DRAW:  stmt_move_draw(5); return;   // DRAW = PLOT 5
            case KW_CLG:   lex_next(); con_clg();   return;
            case KW_SAVE:  stmt_save();    return;
            case KW_LOAD:  stmt_load();    return;
            case KW_DELETE:stmt_delete();  return;
            case KW_CAT:
            case KW_DIR:   lex_next(); stg_dir(); return;
            case KW_PROC:  call_proc(0, 0); return;
            case KW_ENDPROC: g_return = 1; return;
            case KW_LOCAL: stmt_local();   return;
            case KW_DEF:   tok = T_EOL;    return;   // DEF lines are skipped in normal flow
            case KW_REM:   tok = T_EOL;    return;   // ignore rest of line
            case KW_END:   g_stop = 1;     return;
            case KW_ON:      stmt_on();      return;
            case KW_READ:    stmt_read();    return;
            case KW_RESTORE: stmt_restore(); return;
            case KW_VDU:     stmt_vdu();     return;
            case KW_DATA:    tok = T_EOL;    return;   // DATA lines are skipped in normal flow
            case KW_STOP:    con_puts("\nSTOP");
                             if (g_runline >= 0) { con_puts(" at line "); con_putn(g_runline); }
                             con_putc('\n'); g_stop = 1; return;
            case KW_TIME: {                            // TIME = expr  (set the centisecond clock)
                lex_next();
                if (tok != T_EQ) { err("Expected '='"); return; }
                lex_next();
                double v = need_num();
                if (g_err) return;
                time_base = (long long)(con_micros() / 10000ULL) - (long long)v;
                return;
            }
            // RUN clears variables (CLR) then runs from the top. It nests another
            // exec_text() sharing the global lexer, so force EOL afterwards.
            case KW_RUN:   lex_next();
                           clear_vars();
                           run_program(0, 0); tok = T_EOL; return;
            case KW_LIST:     stmt_list();     return;
            case KW_RENUMBER: stmt_renumber(); return;
            case KW_AUTO:     stmt_auto();     return;
            case KW_EDIT:     stmt_edit();     return;
            case KW_NEW:   lex_next(); prog_n = 0; clear_vars();
                           for_sp = 0; gosub_sp = 0; return;
            default:       err("That keyword can't be used as a command");  return;
        }
    }
    if (tok == T_VAR) { stmt_let(0); return; }   // implicit assignment
    if (tok == T_EQ) {                           // =<expr> : return a value from an FN
        lex_next();
        value_t v = eval_expr();
        if (g_err) return;
        fn_retval = v;
        g_return = 1;
        return;
    }
    err("Expected a command");
}

// Execute one line of source starting at byte offset `off` (which may hold
// several ':'-separated statements).
static void exec_text(const char *text, int off) {
    cur_text = text;
    lx = text + off;
    lex_next();
    while (tok == T_COLON) lex_next();   // skip leading ':' (single-line PROC body)
    while (!g_err && !g_stop && !g_branch && !g_return && tok != T_EOL) {
        exec_statement();
        if (g_err || g_stop || g_branch || g_return) break;
        if (tok == T_COLON) { lex_next(); continue; }
        if (tok == T_EOL)   break;
        err("Unexpected text after the statement");    // junk after a statement
        break;
    }
}

// Build the PROC/FN definition table by scanning the program for "DEF" lines.
static void scan_defs(void) {
    def_n = 0;
    for (int i = 0; i < prog_n && def_n < DEF_MAX; i++) {
        cur_text = prog[i].text;
        lx = prog[i].text;
        lex_next();
        if (tok != T_KW || tok_kw != KW_DEF) continue;
        lex_next();
        if (tok == T_KW && (tok_kw == KW_PROC || tok_kw == KW_FN)) {
            s_copy(defs[def_n].name, tok_var, NAME_LEN);
            defs[def_n].is_fn = (tok_kw == KW_FN);
            defs[def_n].line  = i;
            defs[def_n].off   = cur_off();        // just after the name ('(' or body)
            def_n++;
        }
    }
}

// Execute program lines starting at (pc,off) until ENDPROC / =<expr> (g_return),
// END (g_stop), end of program, or an error. Used for PROC and FN bodies.
static void run_body(int pc, int off) {
    g_return = 0;
    while (pc >= 0 && pc < prog_n && !g_err && !g_stop && !g_return) {
        cur_line_idx = pc;
        g_runline = prog[pc].num;
        g_branch = 0;
        exec_text(prog[pc].text, off);
        off = 0;
        if (g_err || g_stop || g_return) break;
        if (g_branch) { pc = g_branch_line; off = g_branch_off; }
        else          { pc++; }
    }
}

static void run_program(int start_pc, int start_off) {
    g_stop = 0;
    gosub_sp = 0;
    for_sp = 0;
    repeat_sp = 0;
    while_sp = 0;
    case_sp = 0;
    call_sp = 0;
    local_sp = 0;
    scratch_base = 0;
    data_pc = 0;
    data_off = -1;
    scan_defs();
    int pc  = start_pc;
    int off = start_off;
    while (pc >= 0 && pc < prog_n && !g_err && !g_stop) {
        cur_line_idx = pc;
        g_runline = prog[pc].num;
        g_branch = 0;
        g_return = 0;
        exec_text(prog[pc].text, off);
        off = 0;
        if (g_err || g_stop) break;
        if (g_branch) { pc = g_branch_line; off = g_branch_off; }
        else          { pc++; }
    }
    cur_line_idx = -1;
    g_runline = -1;
    g_stop = 0;
    g_branch = 0;
}

// ---------------------------------------------------------------------------
// PROC / FN call. The current token is KW_PROC or KW_FN with the name in
// tok_var. Evaluates arguments, binds them (plus any LOCALs) over the callee's
// parameters, runs the body, then restores the caller's state. FN returns its
// value (from =<expr>) in *retval.
// ---------------------------------------------------------------------------

#define MAX_ARGS 16

static void call_proc(int is_fn, value_t *retval) {
    char name[NAME_LEN];
    s_copy(name, tok_var, NAME_LEN);
    lex_next();                                  // consume PROC/FN token

    // Evaluate arguments (copy string args into scratch so a later GC can't move them).
    value_t args[MAX_ARGS];
    int argc = 0;
    if (tok == T_LP) {
        lex_next();
        if (tok != T_RP) {
            for (;;) {
                if (argc >= MAX_ARGS) { err("Too many arguments"); return; }
                value_t a = eval_expr();
                if (g_err) return;
                if (a.is_str) a = str_in_scratch(a.str, a.len);
                args[argc++] = a;
                if (tok == T_COMMA) { lex_next(); continue; }
                break;
            }
        }
        if (!expect(T_RP)) return;
    }

    // Locate the definition.
    int d = -1;
    for (int i = 0; i < def_n; i++)
        if (defs[i].is_fn == is_fn && s_eq(defs[i].name, name)) { d = i; break; }
    if (d < 0) { err(is_fn ? "No such FN" : "No such PROC"); return; }
    if (call_sp >= 32) { err("Too many nested PROC calls"); return; }

    // Save the caller's resume state.
    const char *save_lx   = lx;
    const char *save_text = cur_text;
    int save_tok = tok, save_kw = tok_kw, save_line = cur_line_idx;
    int save_br = g_branch, save_brl = g_branch_line, save_bro = g_branch_off;
    int save_runline = g_runline;
    double save_num = tok_num;
    char save_var[NAME_LEN]; s_copy(save_var, tok_var, NAME_LEN);
    int save_sbase = scratch_base, save_stop = scratch_top;
    int local_mark = local_sp;

    scratch_base = scratch_top;                  // body temporaries start above the args
    call_sp++;

    // Position at the definition and bind formal parameters to the arguments.
    cur_text = prog[defs[d].line].text;
    lx = prog[defs[d].line].text + defs[d].off;
    lex_next();
    int pi = 0;
    int ok = 1;
    int body_off = defs[d].off;                  // no param list -> body is here
    if (tok == T_LP) {
        lex_next();
        if (tok != T_RP) {
            for (;;) {
                if (tok != T_VAR) { err("Expected a parameter name"); ok = 0; break; }
                char pname[NAME_LEN]; s_copy(pname, tok_var, NAME_LEN);
                lex_next();
                if (pi >= argc) { err("Wrong number of arguments"); ok = 0; break; }
                var_t *pv = var_find(pname);
                if (!pv) { ok = 0; break; }
                local_stack[local_sp].slot = pv;
                local_stack[local_sp].old  = *pv;
                local_sp++;
                value_t a = args[pi++];
                if (name_is_str(pname)) {
                    if (!a.is_str) { err("Type mismatch: numbers and text can't be mixed"); ok = 0; break; }
                    str_store(pv, a.str, a.len);
                } else {
                    if (a.is_str) { err("Type mismatch: numbers and text can't be mixed"); ok = 0; break; }
                    pv->is_str = 0; pv->num = trunc_int(pv->is_int, a.num);
                }
                if (tok == T_COMMA) { lex_next(); continue; }
                break;
            }
        }
        if (ok && tok != T_RP) { err("Expected ')' to close the parameter list"); ok = 0; }
        if (ok) body_off = cur_off();            // lx is just past ')' = body start
    }
    if (ok && pi != argc) { err("Wrong number of arguments"); ok = 0; }

    if (ok) {
        run_body(defs[d].line, body_off);
        if (is_fn && retval && !g_err) *retval = fn_retval;
    }

    // Restore locals (parameters + any LOCALs declared in the body).
    while (local_sp > local_mark) {
        local_sp--;
        *local_stack[local_sp].slot = local_stack[local_sp].old;
    }

    // Restore the caller's state. Keep scratch_top for FN so the return value
    // (which lives in scratch) survives; rewind it for PROC.
    call_sp--;
    scratch_base = save_sbase;
    if (!is_fn) scratch_top = save_stop;
    g_return = 0;
    lx = save_lx; cur_text = save_text;
    tok = save_tok; tok_kw = save_kw; tok_num = save_num;
    s_copy(tok_var, save_var, NAME_LEN);
    cur_line_idx = save_line; g_runline = save_runline;
    g_branch = save_br; g_branch_line = save_brl; g_branch_off = save_bro;
}

// ---------------------------------------------------------------------------
// Top-level line handling and REPL
// ---------------------------------------------------------------------------

// Returns 1 if the line was executed immediately, 0 if it was stored.
static int process_line(char *line) {
    char *p = line;
    while (is_space(*p)) p++;
    if (is_digit(*p)) {                         // "<num> <text>" -> store/replace
        int num = 0;
        while (is_digit(*p)) { num = num * 10 + (*p - '0'); p++; }
        while (is_space(*p)) p++;
        prog_store(num, p);
        return 0;
    }
    // Immediate mode: execute the typed line directly.
    scan_defs();                                // so immediate PROC/FN calls resolve
    cur_line_idx = -1;
    g_runline = -1;
    g_branch = 0;
    g_return = 0;
    scratch_base = 0;
    exec_text(p, 0);
    // A direct "GOTO/GOSUB <line>" starts the program running from there.
    if (!g_err && g_branch) run_program(g_branch_line, g_branch_off);
    return 1;
}

void basic_init(void) {
    prog_n = 0;
    var_n = 0;
    g_err = 0;
    g_runline = -1;
    cur_line_idx = -1;
    g_stop = 0;
    g_branch = 0;
    gosub_sp = 0;
    for_sp = 0;
    repeat_sp = 0;
    call_sp = 0;
    local_sp = 0;
    def_n = 0;
    g_return = 0;
    scratch_top = 0;
    scratch_base = 0;
    data_pc = 0;
    data_off = -1;
    time_base = 0;
    clear_vars();
}

void basic_repl(void) {
    static char line[LINE_LEN];
    // Boot logo (target/QEMU only). When it shows the logo it prints the banner
    // beside it and returns 1; otherwise we print the banner ourselves.
    if (!con_splash("BerryBasic (C) 2026 fritzone"))
        con_puts("BerryBasic (C) 2026 fritzone\n\n");
    for (;;) {
        const char *prompt = ">";               // BBC BASIC prompt
        int pre = 0;                             // editable prefill already in `line`
        int was_auto = g_auto_active;            // auto-numbering this line?
        if (g_auto_active) {                     // AUTO: offer the next line number
            prompt = "";
            pre = uint_to_str(line, g_auto_num);
            line[pre++] = ' ';
            line[pre] = 0;
        } else if (g_prefill_len) {              // EDIT: offer the recalled line
            s_copy(line, g_prefill, LINE_LEN);
            pre = g_prefill_len;
            g_prefill_len = 0;
        } else {
            line[0] = 0;
        }
        int n = con_getline_ed(line, LINE_LEN, pre, prompt);
        if (n < 0) return;                      // host EOF
        if (was_auto) {                          // empty entry (just the number) leaves AUTO
            const char *q = line;
            while (is_digit(*q)) q++;
            while (is_space(*q)) q++;
            if (*q == 0) { g_auto_active = 0; continue; }
        }
        g_err = 0;
        g_stop = 0;
        process_line(line);
        if (was_auto && g_auto_active && !g_err) g_auto_num += g_auto_step;
    }
}
