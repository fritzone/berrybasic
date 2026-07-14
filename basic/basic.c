#include <stdint.h>
#include "console.h"
#include "storage.h"
#include "seed.h"
#include "image.h"
#include "sound.h"
#include "gpio.h"
#include "i2c.h"
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

// Structured error handling (TRY/CATCH). When a TRY handler is active, an error
// is recorded here and its printing is deferred to the handler (or to the end of
// RUN if nothing catches it) instead of being printed immediately by err().
#define ERRMSG_MAX 128
static char g_errmsg[ERRMSG_MAX];  // message of the most recent error (for ERR$)
static int  g_errcode = 0;         // numeric code (for ERR): 0 = none / built-in
static int  g_errline = -1;        // line the error happened on (for deferred report)
static int  g_err_reported = 0;    // has the pending error already been printed?
static int  try_sp    = 0;         // depth of active TRY handlers (0 = none)

static void set_errmsg(const char *msg) {
    int i = 0;
    if (msg) while (msg[i] && i < ERRMSG_MAX - 1) { g_errmsg[i] = msg[i]; i++; }
    g_errmsg[i] = 0;
}

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
    set_errmsg(msg);
    g_errcode = 0;           // built-in error: no user code
    g_errline = g_runline;
    if (try_sp > 0) { g_err_reported = 0; return; }  // a TRY may report it: defer
    con_putc('\n');
    con_puts(msg);
    err_tail();
    g_err_reported = 1;
}

// Two-part error, for a fixed prefix plus a dynamic name, e.g.
// err2("Expected ", "')'") -> "Expected ')' (line 30)".
static void err2(const char *a, const char *b) {
    if (g_err) return;
    g_err = 1;
    set_errmsg(a);
    { int n = 0; while (g_errmsg[n]) n++;             // append b onto the message
      for (int i = 0; b[i] && n < ERRMSG_MAX - 1; i++) g_errmsg[n++] = b[i];
      g_errmsg[n] = 0; }
    g_errcode = 0;
    g_errline = g_runline;
    if (try_sp > 0) { g_err_reported = 0; return; }
    con_putc('\n');
    con_puts(a);
    con_puts(b);
    err_tail();
    g_err_reported = 1;
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

// A program line. `module` is 0 for the main program and 1+ for lines pulled in
// by IMPORT; line-number lookups (GOTO/GOSUB/labels/DATA) are scoped to the
// running line's module, so a module's line numbers may freely overlap the main
// program's. Module lines are appended at RUN and stripped when it ends.
typedef struct { int num; int module; char text[LINE_LEN]; } progline_t;

static progline_t prog[MAX_LINES];
static int        prog_n = 0;            // total lines (main + any imported modules)
static int        main_n = 0;            // main-program lines only = prog[0..main_n)

static int cur_module(void);             // module of the currently executing line

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
    prog[i].module = 0;
    s_copy(prog[i].text, text, LINE_LEN);
    prog_n++;
}

// Resolve a line number within the current module only, so overlapping numbers
// in the main program and an imported module never clash.
static int find_line_index(int num) {
    int m = cur_module();
    for (int i = 0; i < prog_n; i++)
        if (prog[i].module == m && prog[i].num == num) return i;
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

// Memory reserved by `DIM name size`: a byte arena that BASIC hands out real
// addresses into, for use with the ?/!/$ indirection operators and for passing
// buffers to native seeds. Reset (like the variable/array pools) on RUN/NEW.
#define DIM_HEAP_SIZE (256 * 1024)   // room for indirection buffers and sprites (GGET/GPUT)
// 16-aligned so a sprite's pixel area (base+8) is 4-aligned: SPRITETARGET renders
// into it with the graphics driver's aligned 32-bit stores (-mstrict-align).
static unsigned char dim_heap[DIM_HEAP_SIZE] __attribute__((aligned(16)));
static int           dim_top = 0;

static void clear_vars(void) {       // BASIC CLR: scalars + arrays + string heap
    var_n = 0;
    gcheap_top = 0;
    arr_n = 0;
    arr_nums_top = 0;
    arr_strs_top = 0;
    dim_top = 0;
    img_sprite_reset();      // free image-loaded sprites
    gpio_reset();            // header pins -> input, no pull (never leave a load driven)
    i2c_reset();             // release the I2C bus (gpio_reset returned SDA/SCL to inputs)
}

// Reserve `nbytes` from the DIM arena, 8-byte aligned; returns the base address
// (as an integer that fits exactly in a double), or 0 if the arena is full.
static long dim_reserve(int nbytes) {
    dim_top = (dim_top + 7) & ~7;
    if (nbytes < 0 || dim_top + nbytes > DIM_HEAP_SIZE) { err("Out of memory"); return 0; }
    long base = (long)(uintptr_t)&dim_heap[dim_top];
    dim_top += nbytes;
    return base;
}

// Alignment-safe peek/poke (byte-wise, so -mstrict-align never faults). Words
// are little-endian 32-bit; reads sign-extend. `$` strings are CR-terminated.
static long mem_peekb(long a) { return *(volatile unsigned char *)(uintptr_t)a; }
static void mem_pokeb(long a, long v) { *(volatile unsigned char *)(uintptr_t)a = (unsigned char)v; }
static long mem_peekw(long a) {
    const volatile unsigned char *p = (const volatile unsigned char *)(uintptr_t)a;
    unsigned w = (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
    return (long)(int)w;
}
static void mem_pokew(long a, long v) {
    volatile unsigned char *p = (volatile unsigned char *)(uintptr_t)a;
    unsigned w = (unsigned)v;
    p[0] = (unsigned char)w; p[1] = (unsigned char)(w >> 8);
    p[2] = (unsigned char)(w >> 16); p[3] = (unsigned char)(w >> 24);
}

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
    KW_SAVE, KW_LOAD, KW_CAT, KW_DIR, KW_DELETE,                    // storage statements
    KW_MKDIR, KW_CD, KW_RMDIR, KW_PWD,                              // directory statements
    KW_BPUT, KW_CLOSE,                                              // file I/O statements
    KW_SEED,                                                        // load a native seed
    KW_AUTO, KW_RENUMBER, KW_EDIT,                                  // editor commands
    KW_MOUSE,                                                       // MOUSE x,y,b statement
    KW_TRUE, KW_FALSE, KW_POS, KW_VPOS,               // parenless value keywords
    KW_PINS,                                          // PINS : read all header pins at once
    KW_SCREEN, KW_SCREENW, KW_SCREENH,                // SCREEN statement + current-resolution values
    KW_KEYBOARD, KW_KEYBOARDS,                        // KEYBOARD "NO" statement + KEYBOARD$ (current layout)
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
    KW__FIRST_FUNC = KW_ABS, KW__LAST_FUNC = KW_I2CPROBE,
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
    { "EXIT", KW_EXIT }, { "CONTINUE", KW_CONTINUE },
    { "TRY", KW_TRY }, { "CATCH", KW_CATCH }, { "ENDTRY", KW_ENDTRY }, { "RAISE", KW_RAISE },
    { "ERR", KW_ERR }, { "ERR$", KW_ERRS },
    { "CLS",   KW_CLS   }, { "COLOUR", KW_COLOUR }, { "COLOR", KW_COLOUR },
    { "DEF",   KW_DEF   }, { "ENDPROC", KW_ENDPROC }, { "LOCAL", KW_LOCAL },
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
    { "TAB",   KW_TAB   }, { "SPC",  KW_SPC   },
};
static const int kwcount = (int)(sizeof(kwtab) / sizeof(kwtab[0]));

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

// ---------------------------------------------------------------------------
// Native seeds: small chunks of position-independent AArch64 machine code that
// a program loads (SEED) and calls (CALL / CALL$). The blob is copied into a
// page-aligned, executable RAM slot; seeds reach the interpreter only through
// the SeedServices vtable below, which is what keeps them self-contained. See
// seed/seed.h for the ABI.
// ---------------------------------------------------------------------------
#define SEED_MAX        8
#define SEED_SLOT_SIZE  (16 * 1024)
enum { SEED_MAX_ARGS = 16 };

static char       seed_pool[SEED_MAX][SEED_SLOT_SIZE] __attribute__((aligned(4096)));
static int        seed_loaded[SEED_MAX];
static seed_entry seed_entry_ptr[SEED_MAX];

static char g_seed_retstr[MAX_STR];   // string result staged by set_return_str
static int  g_seed_retstr_len;        // -1 = the last call set no string

// --- seed heap -------------------------------------------------------------
// A general-purpose allocator for seeds (BASIC's own string/array storage is
// separate). Classic K&R first-fit over a fixed arena with coalescing on free;
// returns 0 when exhausted. The whole arena is reclaimed at each RUN/NEW, so a
// seed that forgets to free leaks only within the current run. 16-byte aligned
// blocks, suitable for doubles and NEON.
#define SEED_HEAP_SIZE  (2u * 1024u * 1024u)   // adjust here if seeds need more

typedef union seed_hdr_u {
    struct { union seed_hdr_u *next; unsigned size; } s;  // size in header units
    long double _align;                                   // force 16-byte units
} seed_blk;

static seed_blk seed_heap[SEED_HEAP_SIZE / sizeof(seed_blk)];
static seed_blk seed_freelist;        // circular free-list sentinel
static seed_blk *seed_freep;          // 0 until first use / after a reset

static void seed_heap_reset(void) { seed_freep = 0; }     // lazily re-inited

static void seed_heap_init(void) {
    seed_blk *base = seed_heap;
    base->s.size = (unsigned)(sizeof(seed_heap) / sizeof(seed_blk));
    base->s.next = &seed_freelist;
    seed_freelist.s.next = base;
    seed_freelist.s.size = 0;
    seed_freep = &seed_freelist;
}

static void *seed_alloc(unsigned nbytes) {
    if (nbytes == 0) return 0;
    if (!seed_freep) seed_heap_init();
    unsigned need = (nbytes + sizeof(seed_blk) - 1) / sizeof(seed_blk) + 1;  // +header
    seed_blk *prev = seed_freep;
    for (seed_blk *p = prev->s.next; ; prev = p, p = p->s.next) {
        if (p->s.size >= need) {                 // big enough
            if (p->s.size == need) prev->s.next = p->s.next;   // exact: unlink
            else { p->s.size -= need; p += p->s.size; p->s.size = need; }  // carve tail
            seed_freep = prev;
            return (void *)(p + 1);
        }
        if (p == seed_freep) return 0;           // wrapped the whole list: no room
    }
}

static void seed_free(void *ap) {
    if (!ap) return;
    seed_blk *bp = (seed_blk *)ap - 1;
    if (bp < seed_heap || bp >= seed_heap + sizeof(seed_heap) / sizeof(seed_blk))
        return;                                  // ignore a wild pointer
    seed_blk *p = seed_freep;
    while (!(bp > p && bp < p->s.next)) {         // find the insertion point
        if (p >= p->s.next && (bp > p || bp < p->s.next)) break;  // at an end
        p = p->s.next;
    }
    if (bp + bp->s.size == p->s.next) {           // coalesce with the next block
        bp->s.size += p->s.next->s.size;
        bp->s.next  = p->s.next->s.next;
    } else bp->s.next = p->s.next;
    if (p + p->s.size == bp) {                     // coalesce with the previous block
        p->s.size += bp->s.size;
        p->s.next  = bp->s.next;
    } else p->s.next = bp;
    seed_freep = p;
}

// Usable payload bytes of an allocated block (its header records the size).
static unsigned seed_block_bytes(void *ap) {
    seed_blk *bp = (seed_blk *)ap - 1;
    if (bp < seed_heap || bp >= seed_heap + sizeof(seed_heap) / sizeof(seed_blk))
        return 0;
    return (bp->s.size - 1) * (unsigned)sizeof(seed_blk);
}

// realloc: grow or shrink a block, preserving its contents. A shrink (or a grow
// that already fits the rounded-up block) keeps the same pointer; otherwise a new
// block is allocated, the old bytes copied, and the old block freed. On failure
// the original block is left untouched and 0 is returned (standard semantics).
static void *seed_realloc(void *ap, unsigned nbytes) {
    if (!ap) return seed_alloc(nbytes);
    if (nbytes == 0) { seed_free(ap); return 0; }
    unsigned cur = seed_block_bytes(ap);
    if (cur == 0) return 0;                      // wild pointer
    if (nbytes <= cur) return ap;                // already fits
    void *np = seed_alloc(nbytes);
    if (!np) return 0;                           // old block stays valid
    const char *s = ap; char *d = np;
    for (unsigned i = 0; i < cur; i++) d[i] = s[i];
    seed_free(ap);
    return np;
}

// aligned allocation: return a block whose payload is `alignment`-aligned, still
// freeable with the ordinary free. Blocks are already 16-aligned, so smaller
// alignments are plain allocs; for larger ones we over-allocate, split off the
// unaligned prefix as its own free block, and hand back the aligned remainder
// (which carries a normal block header, so free/realloc work on it unchanged).
static void *seed_alloc_aligned(unsigned alignment, unsigned nbytes) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return 0;  // need pow2
    if (alignment <= sizeof(seed_blk)) return seed_alloc(nbytes);
    void *raw = seed_alloc(nbytes + alignment);
    if (!raw) return 0;
    uintptr_t r = (uintptr_t)raw;
    uintptr_t a = (r + (alignment - 1)) & ~((uintptr_t)alignment - 1);
    if (a == r) return raw;                      // already aligned
    seed_blk *rawh = (seed_blk *)raw - 1;
    unsigned units1 = (unsigned)((a - r) / sizeof(seed_blk));   // unaligned prefix
    seed_blk *ah = rawh + units1;                // header for the aligned block
    ah->s.size   = rawh->s.size - units1;
    rawh->s.size = units1;
    seed_free(raw);                              // return the prefix to the heap
    return (void *)(ah + 1);                     // == a, with a valid header at a-16
}

// ===========================================================================
// Collections: DICT (string-keyed map), LIST (growable sequence), TREE (binary
// search tree keyed by number). Each is a heap object (built on the general
// allocator above, so it is wiped clean on RUN) referred to by a small integer
// handle. Every stored value is tagged number-or-string, so one collection can
// hold either; the $-suffixed words (DICTGET$, POP$, ...) read string values.
// ===========================================================================

// A stored value: a number, or a string copied into the general heap.
typedef struct { int is_str; double num; char *str; int len; } cval_t;

static void cval_clear(cval_t *c) {
    if (c->is_str && c->str) seed_free(c->str);
    c->is_str = 0; c->num = 0; c->str = 0; c->len = 0;
}
// Overwrite *c with the BASIC value v (a string is copied into the heap so it
// survives independently of BASIC's own GC heap). Returns 0 (and raises) on OOM.
static int cval_store(cval_t *c, value_t v) {
    char *p = 0;
    if (v.is_str && v.len > 0) {
        p = (char *)seed_alloc((unsigned)v.len);
        if (!p) { err("Out of memory"); return 0; }
        for (int i = 0; i < v.len; i++) p[i] = v.str[i];
    }
    cval_clear(c);
    if (v.is_str) { c->is_str = 1; c->str = p; c->len = v.len; }
    else          { c->num = v.num; }
    return 1;
}
// Read a stored value as a typed BASIC value. The numeric/string variants raise
// a type mismatch if the stored value is the other kind (like A vs A$).
static value_t cval_num(cval_t *c) {
    if (c->is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
    return v_num(c->num);
}
static value_t cval_strv(cval_t *c) {
    if (!c->is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
    return str_in_scratch(c->str, c->len);
}

// --- LIST: a growable array of values --------------------------------------
typedef struct { cval_t *item; int len, cap; } list_t;

static int list_reserve(list_t *L, int need) {
    if (need <= L->cap) return 1;
    int cap = L->cap ? L->cap * 2 : 8;
    while (cap < need) cap *= 2;
    cval_t *n = (cval_t *)seed_realloc(L->item, (unsigned)(cap * sizeof(cval_t)));
    if (!n) { err("Out of memory"); return 0; }
    L->item = n; L->cap = cap; return 1;
}
static int list_ins(list_t *L, int i, value_t v) {   // insert before index i (0..len)
    if (i < 0 || i > L->len) { err("Index out of range"); return 0; }
    if (!list_reserve(L, L->len + 1)) return 0;
    cval_t tmp = {0, 0, 0, 0};
    if (!cval_store(&tmp, v)) return 0;               // may OOM before we shift
    for (int k = L->len; k > i; k--) L->item[k] = L->item[k - 1];
    L->item[i] = tmp;
    L->len++;
    return 1;
}
static void list_del(list_t *L, int i) {
    if (i < 0 || i >= L->len) { err("Index out of range"); return; }
    cval_clear(&L->item[i]);
    for (int k = i; k < L->len - 1; k++) L->item[k] = L->item[k + 1];
    L->len--;
}

// --- DICT: an insertion-ordered array of (key, value) entries --------------
typedef struct { char *key; int klen; cval_t val; } dent_t;
typedef struct { dent_t *e; int len, cap; } dict_t;

static int dict_find(dict_t *D, const char *key, int klen) {
    for (int i = 0; i < D->len; i++) {
        if (D->e[i].klen != klen) continue;
        int eq = 1;
        for (int k = 0; k < klen; k++) if (D->e[i].key[k] != key[k]) { eq = 0; break; }
        if (eq) return i;
    }
    return -1;
}
static int dict_set(dict_t *D, const char *key, int klen, value_t v) {
    int i = dict_find(D, key, klen);
    if (i >= 0) return cval_store(&D->e[i].val, v);   // update in place
    if (D->len + 1 > D->cap) {
        int cap = D->cap ? D->cap * 2 : 8;
        dent_t *n = (dent_t *)seed_realloc(D->e, (unsigned)(cap * sizeof(dent_t)));
        if (!n) { err("Out of memory"); return 0; }
        D->e = n; D->cap = cap;
    }
    char *kc = 0;
    if (klen > 0) {
        kc = (char *)seed_alloc((unsigned)klen);
        if (!kc) { err("Out of memory"); return 0; }
        for (int k = 0; k < klen; k++) kc[k] = key[k];
    }
    dent_t *e = &D->e[D->len];
    e->key = kc; e->klen = klen;
    e->val.is_str = 0; e->val.num = 0; e->val.str = 0; e->val.len = 0;
    if (!cval_store(&e->val, v)) { if (kc) seed_free(kc); return 0; }
    D->len++;
    return 1;
}
static void dict_del(dict_t *D, const char *key, int klen) {
    int i = dict_find(D, key, klen);
    if (i < 0) return;
    if (D->e[i].key) seed_free(D->e[i].key);
    cval_clear(&D->e[i].val);
    for (int k = i; k < D->len - 1; k++) D->e[k] = D->e[k + 1];
    D->len--;
}

// --- TREE: a binary search tree keyed by number ----------------------------
typedef struct tnode { double key; cval_t val; struct tnode *l, *r; } tnode_t;
typedef struct { tnode_t *root; int count; } tree_t;

static tnode_t *tree_find(tree_t *T, double key) {
    tnode_t *c = T->root;
    while (c) { if (key == c->key) return c; c = key < c->key ? c->l : c->r; }
    return 0;
}
static int tree_set(tree_t *T, double key, value_t v) {
    tnode_t **link = &T->root, *par = 0;
    while (*link) {
        par = *link;
        if (key == par->key) return cval_store(&par->val, v);   // update
        link = key < par->key ? &par->l : &par->r;
    }
    tnode_t *n = (tnode_t *)seed_alloc((unsigned)sizeof(tnode_t));
    if (!n) { err("Out of memory"); return 0; }
    n->key = key; n->l = 0; n->r = 0;
    n->val.is_str = 0; n->val.num = 0; n->val.str = 0; n->val.len = 0;
    if (!cval_store(&n->val, v)) { seed_free(n); return 0; }
    *link = n; T->count++;
    return 1;
}
static void tree_del(tree_t *T, double key) {
    tnode_t *cur = T->root, *par = 0;
    while (cur && cur->key != key) { par = cur; cur = key < cur->key ? cur->l : cur->r; }
    if (!cur) return;                                 // not found
    if (cur->l && cur->r) {                           // two children: use successor
        tnode_t *sp = cur, *s = cur->r;
        while (s->l) { sp = s; s = s->l; }
        cval_clear(&cur->val);
        cur->key = s->key; cur->val = s->val;         // move successor's payload up
        s->val.is_str = 0; s->val.str = 0;            // ...without a double free
        cur = s; par = sp;                            // now splice out s (<=1 child)
    }
    tnode_t *child = cur->l ? cur->l : cur->r;
    if (!par) T->root = child;
    else if (par->l == cur) par->l = child;
    else par->r = child;
    cval_clear(&cur->val);
    seed_free(cur);
    T->count--;
}
static tnode_t *tree_edge(tree_t *T, int rightmost) {  // min (0) or max (1) node
    tnode_t *c = T->root;
    if (!c) return 0;
    while (rightmost ? c->r : c->l) c = rightmost ? c->r : c->l;
    return c;
}
// The idx-th node in ascending key order (0-based). Iterative in-order walk with
// an explicit heap stack, so a degenerate (deep) tree can't overflow the C stack.
static tnode_t *tree_index(tree_t *T, int idx) {
    if (idx < 0 || idx >= T->count) { err("Index out of range"); return 0; }
    tnode_t **stk = (tnode_t **)seed_alloc((unsigned)(T->count * sizeof(tnode_t *)));
    if (!stk) { err("Out of memory"); return 0; }
    int sp = 0, seen = -1;
    tnode_t *cur = T->root, *res = 0;
    while (cur || sp) {
        while (cur) { stk[sp++] = cur; cur = cur->l; }
        cur = stk[--sp];
        if (++seen == idx) { res = cur; break; }
        cur = cur->r;
    }
    seed_free(stk);
    return res;
}

// --- handle pool ------------------------------------------------------------
#define COLL_MAX 64
enum { CT_FREE = 0, CT_DICT, CT_LIST, CT_TREE };
static struct { int type; void *obj; } colls[COLL_MAX];

// Drop every handle. The objects' memory lives in the general heap, which is
// wiped by seed_heap_reset() on RUN/NEW, so we just clear the table alongside it.
static void coll_reset(void) {
    for (int i = 0; i < COLL_MAX; i++) { colls[i].type = CT_FREE; colls[i].obj = 0; }
}
static double coll_new(int type, unsigned objsize) {
    for (int i = 0; i < COLL_MAX; i++) if (colls[i].type == CT_FREE) {
        char *o = (char *)seed_alloc(objsize);
        if (!o) { err("Out of memory"); return 0; }
        for (unsigned k = 0; k < objsize; k++) o[k] = 0;
        colls[i].type = type; colls[i].obj = o;
        return (double)(i + 1);
    }
    err("Too many collections");
    return 0;
}
// Resolve a handle, requiring a particular type (0 = any). Raises on a bad or
// wrong-typed handle and returns 0.
static void *coll_get(double h, int type) {
    int i = (int)h;
    if (i < 1 || i > COLL_MAX || colls[i - 1].type == CT_FREE) { err("Not a collection"); return 0; }
    if (type && colls[i - 1].type != type) {
        err(type == CT_DICT ? "Not a dictionary" : type == CT_LIST ? "Not a list" : "Not a tree");
        return 0;
    }
    return colls[i - 1].obj;
}
static int coll_size_of(int type, void *o) {
    if (type == CT_LIST) return ((list_t *)o)->len;
    if (type == CT_DICT) return ((dict_t *)o)->len;
    if (type == CT_TREE) return ((tree_t *)o)->count;
    return 0;
}

// --- service callbacks the seed may invoke (names are uppercase + suffix) ---
static void svc_putc(int c)                       { con_putc((char)c); }
static void svc_puts(const char *s, int len)      { con_putsn(s, len); }
static int  svc_getkey(void)                      { return con_getkey(); }
static int  svc_inkey(int cs)                     { return con_inkey(cs); }

// A key that the ON KEY event consumed while detecting the press, held for the
// handler (or the next GET) to read. GET/INKEY go through these wrappers so the
// key that triggered the event is the one the handler reads back.
static int g_pending_key = -1;
static int bas_getkey(void) {
    if (g_pending_key >= 0) { int k = g_pending_key; g_pending_key = -1; return k; }
    return con_getkey();
}
static int bas_inkey(int cs) {
    if (g_pending_key >= 0) { int k = g_pending_key; g_pending_key = -1; return k; }
    return con_inkey(cs);
}

static int svc_get_num(const char *name, double *out) {
    for (int i = 0; i < var_n; i++)
        if (s_eq(vars[i].name, name) && !vars[i].is_str) { *out = vars[i].num; return 1; }
    *out = 0;
    return 0;
}
static void svc_set_num(const char *name, double val) {
    var_t *v = var_find(name);
    if (v && !v->is_str) v->num = trunc_int(v->is_int, val);
}
static int svc_get_str(const char *name, char *buf, int buflen) {
    for (int i = 0; i < var_n; i++)
        if (s_eq(vars[i].name, name) && vars[i].is_str) {
            int n = vars[i].s.slen, c = n < buflen ? n : buflen;
            for (int k = 0; k < c; k++) buf[k] = vars[i].s.sptr[k];
            return n;                              // full length, even if truncated
        }
    return 0;
}
static void svc_set_str(const char *name, const char *buf, int len) {
    var_t *v = var_find(name);
    if (v && v->is_str) str_store(v, buf, len);
}
static double *svc_num_array(const char *name, int *out_len) {
    arr_t *a = arr_find(name);
    if (!a || a->is_str) { if (out_len) *out_len = 0; return 0; }
    if (out_len) *out_len = a->total;
    return &arr_nums[a->off];                      // pool never moves: safe to hand out
}
static void svc_set_return_str(const char *buf, int len) {
    if (len > MAX_STR) len = MAX_STR;
    for (int i = 0; i < len; i++) g_seed_retstr[i] = buf[i];
    g_seed_retstr_len = len;
}
static uint32_t svc_time_cs(void) { return (uint32_t)(con_micros() / 10000ULL); }
static void *svc_alloc(unsigned nbytes) { return seed_alloc(nbytes); }
static void  svc_free(void *ptr)        { seed_free(ptr); }
static void *svc_realloc(void *ptr, unsigned nbytes) { return seed_realloc(ptr, nbytes); }
static void *svc_alloc_aligned(unsigned a, unsigned n) { return seed_alloc_aligned(a, n); }

// GPIO passthroughs (see gpio.h). The driver validates the pin range itself, so
// these are thin; on the host build every gpio_* is a stub and gpio_available()
// is 0, which a seed can test via svc->gpio_avail().
static int  svc_gpio_avail(void)                       { return gpio_available(); }
static int  svc_gpio_mode(int pin, int mode, int alt)  { return gpio_set_mode(pin, mode, alt); }
static int  svc_gpio_pull(int pin, int pull)           { return gpio_set_pull(pin, pull); }
static void svc_gpio_write(int pin, int level)         { gpio_write(pin, level); }
static int  svc_gpio_read(int pin)                     { return gpio_read(pin); }
static void svc_gpio_set(uint32_t mask)                { gpio_set_mask(mask); }
static void svc_gpio_clr(uint32_t mask)                { gpio_clr_mask(mask); }
static uint32_t svc_gpio_level(void)                   { return gpio_read_all(); }
static int  svc_gpio_wait(int pin, int edge, int cs)   { return gpio_wait_edge(pin, edge, cs); }

// SD-card files: thin adapters over the storage channel API (see storage.h),
// which the seed <stdio.h> is built on. All of this shares the file channels and
// filesystem (long names included) with BASIC's OPENIN/OPENOUT.
static int svc_file_open(const char *name, int mode) {
    int m = (mode == SEED_FOPEN_WRITE)  ? STG_M_WRITE
          : (mode == SEED_FOPEN_UPDATE) ? STG_M_UPDATE : STG_M_READ;
    return stg_open(name, m);                         // channel > 0, or 0 on failure
}
static int svc_file_close(int fh) { return stg_close(fh); }
static int svc_file_read(int fh, void *buf, int n) {
    unsigned char *p = buf;
    int i = 0;
    for (; i < n; i++) { int b = stg_getb(fh); if (b < 0) break; p[i] = (unsigned char)b; }
    return i;                                         // short count at EOF (stdio semantics)
}
static int svc_file_write(int fh, const void *buf, int n) {
    const unsigned char *p = buf;
    for (int i = 0; i < n; i++) { int r = stg_putb(fh, p[i]); if (r < 0) return i > 0 ? i : r; }
    return n;
}
static long svc_file_seek(int fh, long off, int whence) {
    long base = (whence == 1) ? stg_tell(fh) : (whence == 2) ? stg_size(fh) : 0;
    if (base < 0) return base;
    long pos = base + off;
    int r = stg_seek(fh, pos);
    return r < 0 ? (long)r : pos;
}
static long svc_file_size(int fh)   { return stg_size(fh); }
static int  svc_file_eof(int fh)    { return stg_eof(fh); }
static int  svc_file_remove(const char *name) { return stg_delete(name); }

// Format a double exactly as PRINT/STR$ do, so a seed's printf %f matches BASIC.
static int svc_fmt_num(double v, char *out) { return dbl_to_str(out, v); }

static const SeedServices g_svc = {
    SEED_ABI_VERSION,
    svc_putc, svc_puts, svc_getkey, svc_inkey,
    svc_get_num, svc_set_num, svc_get_str, svc_set_str,
    svc_num_array, svc_set_return_str, svc_time_cs,
    svc_alloc, svc_free,
    svc_realloc, svc_alloc_aligned,
    svc_gpio_avail, svc_gpio_mode, svc_gpio_pull, svc_gpio_write, svc_gpio_read,
    svc_gpio_set, svc_gpio_clr, svc_gpio_level, svc_gpio_wait,
    svc_file_open, svc_file_close, svc_file_read, svc_file_write,
    svc_file_seek, svc_file_size, svc_file_eof, svc_file_remove,
    svc_fmt_num,
};

// Parse "handle [, arg ...]" (tok at the handle, stops on the first non-comma
// token), invoke the seed, and return its numeric result. String arguments are
// snapshotted into scratch so they stay valid even if the seed triggers GC by
// writing a variable. g_seed_retstr[_len] receives any string result.
static double seed_run_collect(void) {
    double h = need_num();
    if (g_err) return 0;
    int slot = (int)h - 1;
    if (slot < 0 || slot >= SEED_MAX || !seed_loaded[slot]) { err("No such seed"); return 0; }

    seed_arg argv[SEED_MAX_ARGS];
    int argc = 0;
    while (tok == T_COMMA) {
        lex_next();
        if (argc >= SEED_MAX_ARGS) { err("Too many arguments"); return 0; }
        value_t v = eval_expr();
        if (g_err) return 0;
        if (v.is_str) {
            value_t snap = str_in_scratch(v.str, v.len);   // GC-stable copy
            if (g_err) return 0;
            argv[argc].is_str = 1; argv[argc].num = 0;
            argv[argc].str = snap.str; argv[argc].len = snap.len;
        } else {
            argv[argc].is_str = 0; argv[argc].num = v.num;
            argv[argc].str = 0; argv[argc].len = 0;
        }
        argc++;
    }

    g_seed_retstr_len = -1;
    double ret = 0;
    if (seed_invoke(seed_entry_ptr[slot], &g_svc, argv, argc, &ret) != 0) {
        err("Native seeds run on the Pi, not the host build");
        return 0;
    }
    return ret;
}

// Byte-equality of two n-byte spans (used by the string search/replace helpers).
static int mem_eq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

// Parse an array reference written with empty parentheses, NAME() , as used by
// SPLIT and JOIN$. Fills *name and leaves the caller to find/create the array.
static int parse_array_ref(char *name) {
    if (tok != T_VAR) { err("Expected an array name like parts$()"); return 0; }
    s_copy(name, tok_var, NAME_LEN);
    lex_next();
    if (!expect(T_LP)) return 0;
    return expect(T_RP);
}

static int gpio_guard(void);            // defined with the GPIO statements below

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

        // File I/O value functions. OPEN* take a filename string; the BGET/EOF/
        // EXT/PTR family take a '#channel'.
        case KW_OPENIN:  { value_t s = need_str(); if (g_err) return v_num(0);
                           char nm[64]; copy_fname(s, nm, sizeof nm);
                           return v_num((double)stg_open(nm, STG_M_READ)); }
        case KW_OPENOUT: { value_t s = need_str(); if (g_err) return v_num(0);
                           char nm[64]; copy_fname(s, nm, sizeof nm);
                           return v_num((double)stg_open(nm, STG_M_WRITE)); }
        case KW_OPENUP:  { value_t s = need_str(); if (g_err) return v_num(0);
                           char nm[64]; copy_fname(s, nm, sizeof nm);
                           return v_num((double)stg_open(nm, STG_M_UPDATE)); }
        case KW_BGET: { int ch = read_channel(); if (g_err) return v_num(0);
                        return v_num((double)stg_getb(ch)); }   // -1 at end of file
        case KW_EOF:  { int ch = read_channel(); if (g_err) return v_num(0);
                        return v_num(stg_eof(ch) ? -1.0 : 0.0); }
        case KW_EXT:  { int ch = read_channel(); if (g_err) return v_num(0);
                        return v_num((double)stg_size(ch)); }
        case KW_PTR:  { int ch = read_channel(); if (g_err) return v_num(0);
                        return v_num((double)stg_tell(ch)); }
        // DIROPEN "path" begins scanning a directory. Returns TRUE if the scan
        // started, FALSE if the directory could not be opened. Walk it with
        // DIRNEXT, read each entry via DIRNAME$/DIRSIZE/DIRTYPE/DIRDATE$/DIRTIME$.
        case KW_DIROPEN: { value_t s = need_str(); if (g_err) return v_num(0);
                           char nm[128]; copy_fname(s, nm, sizeof nm);
                           g_dir_valid = 0;
                           return v_num(stg_diropen(nm) == 0 ? -1.0 : 0.0); }
    }

    // All other functions keep the parenthesised form: FUNC(arg[,arg...]).
    if (!expect(T_LP)) return v_num(0);
    value_t result = v_num(0);
    switch (fn) {
        case KW_INKEY:  { int n = (int)need_num(); result = v_num((double)bas_inkey(n)); break; }
        case KW_INKEYS: { int n = (int)need_num(); int k = bas_inkey(n);
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

        // --- Collections (see the NEW* creators) --------------------------------
        case KW_SIZE: { double h = need_num(); if (g_err) break;
                        void *o = coll_get(h, 0); if (!o) break;
                        result = v_num(coll_size_of(colls[(int)h - 1].type, o)); break; }
        case KW_DICTGET: case KW_DICTGETS: {
            double h = need_num(); if (!expect(T_COMMA)) break;
            value_t k = need_str(); if (g_err) break;
            dict_t *D = (dict_t *)coll_get(h, CT_DICT); if (!D) break;
            int i = dict_find(D, k.str, k.len);
            if (i < 0) result = (fn == KW_DICTGETS) ? v_str(0, 0) : v_num(0);   // absent: ""/0
            else       result = (fn == KW_DICTGETS) ? cval_strv(&D->e[i].val) : cval_num(&D->e[i].val);
            break; }
        case KW_DICTHAS: {
            double h = need_num(); if (!expect(T_COMMA)) break;
            value_t k = need_str(); if (g_err) break;
            dict_t *D = (dict_t *)coll_get(h, CT_DICT); if (!D) break;
            result = v_num(dict_find(D, k.str, k.len) >= 0 ? -1.0 : 0.0); break; }
        case KW_DICTKEYS: {
            double h = need_num(); if (!expect(T_COMMA)) break;
            int i = (int)need_num(); if (g_err) break;
            dict_t *D = (dict_t *)coll_get(h, CT_DICT); if (!D) break;
            if (i < 0 || i >= D->len) { err("Index out of range"); break; }
            result = str_in_scratch(D->e[i].key, D->e[i].klen); break; }
        case KW_POP: case KW_POPS: {
            double h = need_num(); if (g_err) break;
            list_t *L = (list_t *)coll_get(h, CT_LIST); if (!L) break;
            if (L->len == 0) { err("List is empty"); break; }
            cval_t *c = &L->item[L->len - 1];
            if (fn == KW_POPS) result = cval_strv(c); else result = cval_num(c);
            if (g_err) break;                                  // wrong type: leave item in place
            cval_clear(c); L->len--; break; }
        case KW_LISTGET: case KW_LISTGETS: {
            double h = need_num(); if (!expect(T_COMMA)) break;
            int i = (int)need_num(); if (g_err) break;
            list_t *L = (list_t *)coll_get(h, CT_LIST); if (!L) break;
            if (i < 0 || i >= L->len) { err("Index out of range"); break; }
            result = (fn == KW_LISTGETS) ? cval_strv(&L->item[i]) : cval_num(&L->item[i]); break; }
        case KW_TREEGET: case KW_TREEGETS: {
            double h = need_num(); if (!expect(T_COMMA)) break;
            double key = need_num(); if (g_err) break;
            tree_t *T = (tree_t *)coll_get(h, CT_TREE); if (!T) break;
            tnode_t *n = tree_find(T, key);
            if (!n) result = (fn == KW_TREEGETS) ? v_str(0, 0) : v_num(0);       // absent: ""/0
            else    result = (fn == KW_TREEGETS) ? cval_strv(&n->val) : cval_num(&n->val);
            break; }
        case KW_TREEHAS: {
            double h = need_num(); if (!expect(T_COMMA)) break;
            double key = need_num(); if (g_err) break;
            tree_t *T = (tree_t *)coll_get(h, CT_TREE); if (!T) break;
            result = v_num(tree_find(T, key) ? -1.0 : 0.0); break; }
        case KW_TREEMIN: case KW_TREEMAX: {
            double h = need_num(); if (g_err) break;
            tree_t *T = (tree_t *)coll_get(h, CT_TREE); if (!T) break;
            tnode_t *n = tree_edge(T, fn == KW_TREEMAX);
            if (!n) { err("Tree is empty"); break; }
            result = v_num(n->key); break; }
        case KW_TREEKEY: {
            double h = need_num(); if (!expect(T_COMMA)) break;
            int i = (int)need_num(); if (g_err) break;
            tree_t *T = (tree_t *)coll_get(h, CT_TREE); if (!T) break;
            tnode_t *n = tree_index(T, i);
            if (n) result = v_num(n->key);
            break; }
        case KW_I2CPROBE: { int addr = (int)need_num(); if (g_err) break;
                            if (!i2c_available()) { err("I2C needs real Pi hardware"); break; }
                            result = v_num(i2c_probe(addr) ? -1.0 : 0.0); break; }
        case KW_RGB: { int r = (int)need_num(); if (!expect(T_COMMA)) break;
                       int g = (int)need_num(); if (!expect(T_COMMA)) break;
                       int b = (int)need_num(); if (g_err) break;
                       // Tagged (bit 30) so GCOL can tell a packed colour from a
                       // logical colour index; low 24 bits are 0xRRGGBB.
                       long v = 0x40000000L | ((long)(r & 255) << 16)
                              | ((long)(g & 255) << 8) | (long)(b & 255);
                       result = v_num((double)v); break; }
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
        // --- modern string library ------------------------------------------
        case KW_UPPERS: case KW_LOWERS: {           // case conversion
                          value_t s = need_str(); if (g_err) break;
                          if (s.len == 0) { result = v_str(0, 0); break; }
                          char *p = scratch_alloc(s.len); if (!p) break;
                          for (int i = 0; i < s.len; i++) {
                              char c = s.str[i];
                              if (fn == KW_UPPERS) { if (c >= 'a' && c <= 'z') c -= 32; }
                              else                 { if (c >= 'A' && c <= 'Z') c += 32; }
                              p[i] = c;
                          }
                          result = v_str(p, s.len); break; }
        case KW_TRIMS: {                            // strip leading/trailing whitespace
                          value_t s = need_str(); if (g_err) break;
                          int a = 0, b = s.len;
                          while (a < b && is_space(s.str[a])) a++;
                          while (b > a && is_space(s.str[b - 1])) b--;
                          result = str_in_scratch(s.str + a, b - a); break; }
        case KW_REPLACES: {                         // REPLACE$(s,old,new): all occurrences
                          value_t s = need_str(); if (!expect(T_COMMA)) break;
                          value_t o = need_str(); if (!expect(T_COMMA)) break;
                          value_t nw = need_str(); if (g_err) break;
                          if (o.len == 0) { result = str_in_scratch(s.str, s.len); break; }
                          int cnt = 0;
                          for (int i = 0; i + o.len <= s.len; ) {
                              if (mem_eq(s.str + i, o.str, o.len)) { cnt++; i += o.len; } else i++;
                          }
                          long outlen = (long)s.len + (long)cnt * ((long)nw.len - o.len);
                          if (outlen > MAX_STR) { err("Text string is too long"); break; }
                          char *p = outlen > 0 ? scratch_alloc((int)outlen) : 0;
                          if (outlen > 0 && !p) break;
                          int w = 0;
                          for (int i = 0; i < s.len; ) {
                              if (i + o.len <= s.len && mem_eq(s.str + i, o.str, o.len)) {
                                  for (int k = 0; k < nw.len; k++) p[w++] = nw.str[k];
                                  i += o.len;
                              } else p[w++] = s.str[i++];
                          }
                          result = v_str(p, w); break; }
        case KW_CONTAINS: {                         // CONTAINS(s,sub) -> TRUE/FALSE
                          value_t s = need_str(); if (!expect(T_COMMA)) break;
                          value_t sub = need_str(); if (g_err) break;
                          int found = (sub.len == 0);
                          for (int i = 0; !found && i + sub.len <= s.len; i++)
                              if (mem_eq(s.str + i, sub.str, sub.len)) found = 1;
                          result = v_num(found ? -1.0 : 0.0); break; }
        case KW_STARTSWITH: {                       // STARTSWITH(s,prefix) -> TRUE/FALSE
                          value_t s = need_str(); if (!expect(T_COMMA)) break;
                          value_t pre = need_str(); if (g_err) break;
                          int r = (pre.len <= s.len) && mem_eq(s.str, pre.str, pre.len);
                          result = v_num(r ? -1.0 : 0.0); break; }
        case KW_ENDSWITH: {                         // ENDSWITH(s,suffix) -> TRUE/FALSE
                          value_t s = need_str(); if (!expect(T_COMMA)) break;
                          value_t suf = need_str(); if (g_err) break;
                          int r = (suf.len <= s.len) && mem_eq(s.str + (s.len - suf.len), suf.str, suf.len);
                          result = v_num(r ? -1.0 : 0.0); break; }
        case KW_SPLIT: {                            // SPLIT(s,sep,parts$()) -> pieces stored
                          value_t s = need_str(); if (!expect(T_COMMA)) break;
                          value_t sep = need_str(); if (!expect(T_COMMA)) break;
                          if (g_err) break;
                          char sepbuf[MAX_STR]; int seplen = sep.len;
                          for (int i = 0; i < seplen; i++) sepbuf[i] = sep.str[i];
                          char aname[NAME_LEN]; if (!parse_array_ref(aname)) break;
                          if (!name_is_str(aname)) { err("SPLIT needs a string array, e.g. parts$()"); break; }
                          // s stays valid: it lives in scratch, which the GC never moves.
                          int P;                                   // number of pieces
                          if (seplen == 0) P = s.len;              // empty separator: one per char
                          else { int c = 0;
                                 for (int i = 0; i + seplen <= s.len; ) {
                                     if (mem_eq(s.str + i, sepbuf, seplen)) { c++; i += seplen; } else i++; }
                                 P = c + 1; }
                          arr_t *a = arr_find(aname);
                          if (!a) { int counts[1]; counts[0] = P > 0 ? P : 1;
                                    a = arr_create(aname, 1, counts, 1); if (!a) break; }
                          else if (!a->is_str || a->ndim != 1) {
                                    err("SPLIT needs a one-dimensional string array"); break; }
                          int off = a->off, cap = a->total, stored = 0;
                          if (seplen == 0) {
                              for (int i = 0; i < s.len && stored < cap; i++) {
                                  str_store_to(&arr_strs[off + stored], s.str + i, 1);
                                  if (g_err) break;
                                  stored++;
                              }
                          } else {
                              int start = 0;
                              for (int i = 0; i + seplen <= s.len; ) {
                                  if (mem_eq(s.str + i, sepbuf, seplen)) {
                                      if (stored < cap) {
                                          str_store_to(&arr_strs[off + stored], s.str + start, i - start);
                                          if (g_err) break;
                                          stored++;
                                      }
                                      i += seplen; start = i;
                                  } else i++;
                              }
                              if (!g_err && stored < cap) {
                                  str_store_to(&arr_strs[off + stored], s.str + start, s.len - start); stored++; }
                          }
                          if (g_err) break;
                          result = v_num((double)stored); break; }
        case KW_JOINS: {                            // JOIN$(parts$(),sep[,count]) -> joined string
                          char aname[NAME_LEN]; if (!parse_array_ref(aname)) break;
                          if (!name_is_str(aname)) { err("JOIN$ needs a string array, e.g. parts$()"); break; }
                          arr_t *a = arr_find(aname);
                          if (!a || !a->is_str || a->ndim != 1) {
                              err("JOIN$ needs a one-dimensional string array"); break; }
                          if (!expect(T_COMMA)) break;
                          value_t sep = need_str(); if (g_err) break;
                          char sepbuf[MAX_STR]; int seplen = sep.len;
                          for (int i = 0; i < seplen; i++) sepbuf[i] = sep.str[i];
                          int count = a->total;
                          if (tok == T_COMMA) { lex_next(); count = (int)need_num(); if (g_err) break; }
                          if (count < 0) count = 0;
                          if (count > a->total) count = a->total;
                          long outlen = 0;
                          for (int i = 0; i < count; i++) outlen += arr_strs[a->off + i].slen;
                          if (count > 0) outlen += (long)(count - 1) * seplen;
                          if (outlen > MAX_STR) { err("Text string is too long"); break; }
                          char *p = outlen > 0 ? scratch_alloc((int)outlen) : 0;
                          if (outlen > 0 && !p) break;
                          int w = 0;
                          for (int i = 0; i < count; i++) {
                              if (i > 0) for (int k = 0; k < seplen; k++) p[w++] = sepbuf[k];
                              strdesc_t *d = &arr_strs[a->off + i];
                              for (int k = 0; k < d->slen; k++) p[w++] = d->sptr[k];
                          }
                          result = v_str(p, w); break; }
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
        // CALL(handle[,arg...]) -> seed's numeric result.
        // CALL$(handle[,arg...]) -> the string the seed set via set_return_str.
        case KW_CALL:  result = v_num(seed_run_collect()); break;
        case KW_CALLS: { double r = seed_run_collect(); (void)r; if (g_err) break;
                         result = (g_seed_retstr_len >= 0)
                                  ? str_in_scratch(g_seed_retstr, g_seed_retstr_len)
                                  : v_str(0, 0); break; }
        // LOADSPRITE("file") decodes an image into a sprite and returns its
        // address (0 on failure). SPRW/SPRH read the width/height from a sprite's
        // header, so a program can position or scale it.
        case KW_LOADSPRITE: { value_t s = need_str(); if (g_err) break;
                              char nm[64]; copy_fname(s, nm, sizeof nm);
                              result = v_num((double)img_load_sprite(nm)); break; }
        // PEEK(addr) -> the byte at addr (0..255). Familiar alias for ?addr; use
        // ! for a 32-bit word and $ for a string. See also POKE.
        case KW_PEEK: { long a = (long)need_num(); if (g_err) break;
                        result = v_num((double)mem_peekb(a)); break; }
        // PIN(n) -> the level (0/1) of BCM header pin n. The statement form
        // PIN n,v (drive an output) is in exec_statement.
        case KW_PIN: { if (gpio_guard()) break;
                       int p = (int)need_num(); if (g_err) break;
                       if (p < 0 || p > 27) { err("No such pin"); break; }
                       result = v_num((double)gpio_read(p)); break; }
        // PINWAIT(pin, edge, timeout_cs) -> pin when the edge arrives, -1 on
        // timeout. edge: 0 = falling, 1 = rising. timeout in centiseconds.
        case KW_PINWAIT: { if (gpio_guard()) break;
                       int p = (int)need_num(); if (!expect(T_COMMA)) break;
                       int e = (int)need_num(); if (!expect(T_COMMA)) break;
                       int t = (int)need_num(); if (g_err) break;
                       if (p < 0 || p > 27) { err("No such pin"); break; }
                       result = v_num((double)gpio_wait_edge(p, e, t)); break; }
        // EVAL("expr") parses the string as an expression and returns its value
        // (number or string). The interpreter already re-lexes source on the fly,
        // so this just points the lexer at a private copy of the string, runs the
        // ordinary expression evaluator, and restores the lexer. The copy is on
        // the C stack because evaluating the inner expression allocates scratch,
        // which could otherwise overwrite the source we are reading.
        case KW_EVAL: { value_t s = need_str(); if (g_err) break;
                       char buf[MAX_STR + 1];
                       int n = s.len; if (n > MAX_STR) n = MAX_STR;
                       for (int i = 0; i < n; i++) buf[i] = s.str[i];
                       buf[n] = 0;
                       lexstate_t save; lex_save(&save);
                       cur_text = buf; lx = buf; lex_next();
                       result = eval_expr();
                       if (!g_err && tok != T_EOL) err("Syntax error in EVAL string");
                       lex_restore(&save); break; }
        case KW_SPRW: case KW_SPRH: {
                          long a = (long)need_num(); if (g_err) break;
                          long v = 0;
                          if (a) {
                              const unsigned char *p = (const unsigned char *)(uintptr_t)a;
                              if (fn == KW_SPRH) p += 4;
                              v = (long)p[0] | ((long)p[1] << 8) |
                                  ((long)p[2] << 16) | ((long)p[3] << 24);
                          }
                          result = v_num((double)v); break; }
        default: err("Syntax error in expression"); break;
    }
    if (!g_err) expect(T_RP);
    return result;
}

// Read a CR-terminated string from memory (the BBC $ indirection).
static value_t mem_read_str(long a) {
    const unsigned char *p = (const unsigned char *)(uintptr_t)a;
    int n = 0;
    while (n < MAX_STR && p[n] != 0x0D) n++;
    return str_in_scratch((const char *)p, n);
}
static void mem_write_str(long a, const char *s, int len) {
    unsigned char *p = (unsigned char *)(uintptr_t)a;
    if (len > MAX_STR) len = MAX_STR;
    for (int i = 0; i < len; i++) p[i] = (unsigned char)s[i];
    p[len] = 0x0D;                                   // carriage-return terminator
}

static value_t prim_base(void) {
    if (g_err) return v_num(0);
    // Unary indirection: ?addr (byte), !addr (word), $addr (string). The operand
    // is a primary, so ?(A+1) needs parentheses; A?1 is the binary form below.
    if (tok == T_QUERY) { lex_next(); value_t a = prim_base();
        if (g_err) return v_num(0);
        if (a.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        return v_num((double)mem_peekb((long)a.num)); }
    if (tok == T_PLING) { lex_next(); value_t a = prim_base();
        if (g_err) return v_num(0);
        if (a.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        return v_num((double)mem_peekw((long)a.num)); }
    if (tok == T_DOLLAR) { lex_next(); value_t a = prim_base();
        if (g_err) return v_num(0);
        if (a.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        return mem_read_str((long)a.num); }
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
        if (tok == T_LP) {                       // function call or array element
            // A `name(args)` where `name` is a defined function is a call; only
            // if no such function exists does it mean an array subscript.
            if (find_fn_def(name) >= 0) {
                value_t v = v_num(0);
                call_named(1, name, &v);
                return v;
            }
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
            case KW_NEWDICT: lex_next(); return v_num(coll_new(CT_DICT, sizeof(dict_t)));
            case KW_NEWLIST: lex_next(); return v_num(coll_new(CT_LIST, sizeof(list_t)));
            case KW_NEWTREE: lex_next(); return v_num(coll_new(CT_TREE, sizeof(tree_t)));
            case KW_POS:   lex_next(); return v_num(con_pos());
            case KW_VPOS:  lex_next(); return v_num(con_vpos());
            case KW_ERR:   lex_next(); return v_num(g_errcode);
            case KW_ERRS:  { lex_next();
                             int n = 0; while (g_errmsg[n]) n++;
                             return str_in_scratch(g_errmsg, n); }
            case KW_PINS:  lex_next();
                           if (!gpio_available()) { err("GPIO needs the Pi, not the host build"); return v_num(0); }
                           return v_num((double)gpio_read_all());
            case KW_SCREENW: lex_next(); return v_num(con_screen_w());
            case KW_SCREENH: lex_next(); return v_num(con_screen_h());
            case KW_KEYBOARDS: { lex_next(); const char *c = con_get_keyboard();
                                 int n = 0; while (c[n]) n++;
                                 return str_in_scratch(c, n); }
            case KW_MOUSEX: { int x; lex_next(); con_mouse(&x, 0, 0); return v_num(x); }
            case KW_MOUSEY: { int y; lex_next(); con_mouse(0, &y, 0); return v_num(y); }
            case KW_MOUSEB: { int b; lex_next(); con_mouse(0, 0, &b); return v_num(b); }
            case KW_TIME:  lex_next();
                           return v_num((double)((long long)(con_micros() / 10000ULL) - time_base));
            case KW_FN:  { value_t v = v_num(0); call_proc(1, &v); return v; }
            case KW_GET:   lex_next(); return v_num((double)bas_getkey());
            case KW_GETS:{ lex_next(); char ch = (char)bas_getkey(); return str_in_scratch(&ch, 1); }
            // Directory-scan cursor (see DIROPEN). DIRNEXT advances to the next
            // entry, returning TRUE while there is one; the rest read fields of
            // the entry DIRNEXT last landed on.
            case KW_DIRNEXT: { lex_next();
                               int r = stg_dirnext(&g_dirent);
                               if (r < 0) { err("Disk error"); return v_num(0); }
                               g_dir_valid = (r == 1);
                               return v_num(r == 1 ? -1.0 : 0.0); }
            case KW_DIRNAMES: { lex_next();
                                if (!g_dir_valid) return v_str(0, 0);
                                int n = 0; while (g_dirent.name[n]) n++;
                                return str_in_scratch(g_dirent.name, n); }
            case KW_DIRSIZE: lex_next();
                             return v_num(g_dir_valid ? (double)g_dirent.size : 0.0);
            case KW_DIRTYPE: lex_next();
                             return v_num(g_dir_valid && g_dirent.is_dir ? -1.0 : 0.0);
            case KW_DIRDATES: { lex_next(); char b[12];
                                if (!g_dir_valid) return v_str(0, 0);
                                fmt_date(b, g_dirent.year, g_dirent.month, g_dirent.day);
                                return str_in_scratch(b, 10); }
            case KW_DIRTIMES: { lex_next(); char b[8];
                                if (!g_dir_valid) return v_str(0, 0);
                                fmt_time(b, g_dirent.hour, g_dirent.minute);
                                return str_in_scratch(b, 5); }
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

// A primary, plus the binary indirection postfix: base?offset (byte at base+off)
// and base!offset (word at base+off). Binds tighter than the arithmetic ops, so
// P%?I + 1 is (P%?I) + 1, and the offset is itself a primary.
static value_t eval_primary(void) {
    value_t a = prim_base();
    while (!g_err && (tok == T_QUERY || tok == T_PLING)) {
        int op = tok;
        lex_next();
        value_t off = prim_base();
        if (g_err) return a;
        if (a.is_str || off.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        long addr = (long)a.num + (long)off.num;
        a = (op == T_QUERY) ? v_num((double)mem_peekb(addr))
                            : v_num((double)mem_peekw(addr));
    }
    return a;
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
static int  cur_line_idx;        // prog[] index of that line, or -1 if immediate

// The module the executing line belongs to (0 = main program, or immediate mode).
static int cur_module(void) {
    return (cur_line_idx >= 0 && cur_line_idx < prog_n) ? prog[cur_line_idx].module : 0;
}

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

// Loop-type tags for EXIT/CONTINUE. The three loop stacks (for_/repeat_/while_)
// don't record their relative nesting order, so EXIT/CONTINUE find the innermost
// loop by comparing the source positions of each stack's active frames instead.
enum { LOOP_FOR = 1, LOOP_REPEAT, LOOP_WHILE };

// TRY/CATCH handler stack. Each active TRY records where its CATCH clause starts
// and a snapshot of the interpreter's stacks, so catching an error unwinds any
// loops / PROC frames entered since the TRY and resumes cleanly at the handler.
#define TRY_MAX 16
typedef struct {
    int catch_pc, catch_off;       // where the CATCH handler body begins
    int call_sp;                   // PROC/FN depth the TRY sits at
    int for_sp, repeat_sp, while_sp, case_sp, gosub_sp, fn_ret_sp, local_sp;
    int scratch_base, scratch_top;
} try_rec_t;
static try_rec_t try_stack[TRY_MAX];

// Procedures / functions
static int     g_return;         // ENDPROC / END FN / =<expr> ends the current body
static value_t fn_retval;        // value returned from an FN
static int     call_sp;          // PROC/FN recursion depth

// New-style functions (DEF fn NAME(..) / NAME = expr / END fn) return the value
// of a local variable named after the function. This stack holds that variable
// for each active new-style call, so END fn knows what to hand back.
#define FN_RET_MAX 32
static var_t  *fn_ret_slot[FN_RET_MAX];
static int     fn_ret_sp;

#define DEF_MAX 64
// newstyle: 1 = DEF fn/proc NAME (spaced, END fn/proc-terminated); 0 = glued
// DEF FNNAME / DEF PROCNAME (=<expr> / ENDPROC-terminated).
typedef struct { char name[NAME_LEN]; int is_fn; int newstyle; int line; int off; } defrec_t;
static defrec_t defs[DEF_MAX];
static int      def_n;

// Is `name` a defined function (either style)? Returns its defs[] index or -1.
// Lets a bare `name(args)` in an expression be recognised as a call.
static int find_fn_def(const char *name) {
    for (int i = 0; i < def_n; i++)
        if (defs[i].is_fn && s_eq(defs[i].name, name)) return i;
    return -1;
}

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
    int m = cur_module();
    for (int i = 0; i < prog_n; i++) {
        if (prog[i].module != m) continue;
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
static void exec_text(const char *text, int off);   // forward decl (EXEC runs a string)

#define PRINT_FIELD 8     // column width for the ',' separator and TAB alignment

// PRINT items separated by ; (close up), , (next field), ' (newline), TAB(n),
// SPC(n). Column is tracked from the start of this PRINT (assumed column 0).
// --- typed record I/O (PRINT# / INPUT#) -------------------------------------
// Each item written by PRINT# is a self-describing record:
//   number : 0x40, then 8 bytes IEEE-754 double, little-endian
//   string : 0x00, then a 1-byte length (0..255), then that many bytes
// INPUT# reads the tag and reconstructs the value into the target variable.
#define REC_NUM  0x40
#define REC_STR  0x00

static int rec_put_num(int ch, double x) {
    union { double d; unsigned char b[8]; } u; u.d = x;
    if (stg_putb(ch, REC_NUM) < 0) return -1;
    for (int i = 0; i < 8; i++) if (stg_putb(ch, u.b[i]) < 0) return -1;
    return 0;
}
static int rec_put_str(int ch, const char *s, int len) {
    if (len < 0) len = 0;
    if (len > 255) len = 255;
    if (stg_putb(ch, REC_STR) < 0) return -1;
    if (stg_putb(ch, len & 0xFF) < 0) return -1;
    for (int i = 0; i < len; i++) if (stg_putb(ch, (unsigned char)s[i]) < 0) return -1;
    return 0;
}

// PRINT# ch, item, item, ... : write each value as a typed record.
static void stmt_print_file(void) {
    int ch = read_channel();
    if (g_err) return;
    while (tok == T_COMMA || tok == T_SEMI) {
        lex_next();
        if (tok == T_EOL || tok == T_COLON) break;      // trailing separator
        value_t v = eval_expr();
        if (g_err) return;
        int r = v.is_str ? rec_put_str(ch, v.str, v.len) : rec_put_num(ch, v.num);
        if (r < 0) { err("File write error"); return; }
    }
}

// INPUT# ch, var, var, ... : read typed records back into variables/array elements.
static void stmt_input_file(void) {
    int ch = read_channel();
    if (g_err) return;
    while (tok == T_COMMA || tok == T_SEMI) {
        lex_next();
        if (tok != T_VAR) { err("Expected a variable name"); return; }
        char name[NAME_LEN]; s_copy(name, tok_var, NAME_LEN);
        int isstr = name_is_str(name);
        lex_next();
        int is_arr = 0, idx = 0; arr_t *a = 0; var_t *var = 0;
        if (tok == T_LP) { if (!arr_elem(name, isstr, &idx, &a)) return; is_arr = 1; }
        else             { var = var_find(name); if (!var) return; }

        int tag = stg_getb(ch);
        if (tag < 0) { err("End of file"); return; }
        if (tag == REC_NUM) {
            if (isstr) { err("Type mismatch: file record is a number"); return; }
            union { double d; unsigned char b[8]; } u;
            for (int i = 0; i < 8; i++) {
                int by = stg_getb(ch);
                if (by < 0) { err("End of file"); return; }
                u.b[i] = (unsigned char)by;
            }
            if (is_arr) arr_nums[idx] = trunc_int(a->is_int, u.d);
            else        var->num      = trunc_int(var->is_int, u.d);
        } else if (tag == REC_STR) {
            if (!isstr) { err("Type mismatch: file record is a string"); return; }
            int len = stg_getb(ch);
            if (len < 0) { err("End of file"); return; }
            static char sb[MAX_STR];
            for (int i = 0; i < len; i++) {
                int by = stg_getb(ch);
                if (by < 0) { err("End of file"); return; }
                sb[i] = (char)by;
            }
            if (is_arr) str_store_to(&arr_strs[idx], sb, len);
            else        str_store(var, sb, len);
        } else {
            err("File is not a PRINT# record");
            return;
        }
        if (g_err) return;
    }
}

static void stmt_print(void) {
    lex_next();                              // consume PRINT
    if (tok == T_HASH) { stmt_print_file(); return; }
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

    if (tok == T_QUERY || tok == T_PLING) {  // poke: P%?off = v (byte) / P%!off = v (word)
        if (isstr) { err("Type mismatch: numbers and text can't be mixed"); return; }
        int op = tok;
        var_t *bv = var_find(name);
        long base = bv ? (long)bv->num : 0;
        lex_next();
        value_t off = prim_base();           // offset is a primary (tight)
        if (g_err) return;
        if (off.is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
        long addr = base + (long)off.num;
        if (tok != T_EQ) { err("Expected '='"); return; }
        lex_next();
        double v = need_num();
        if (g_err) return;
        if (op == T_QUERY) mem_pokeb(addr, (long)v); else mem_pokew(addr, (long)v);
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

// Unary indirection poke statement: ?addr = v (byte), !addr = v (word),
// $addr = s$ (CR-terminated string). The address is a primary, so use
// parentheses for an arithmetic address: ?(P%+1) = v  (or write P%?1 = v).
static void stmt_poke(void) {
    int op = tok;                            // T_QUERY / T_PLING / T_DOLLAR
    lex_next();
    value_t addrv = prim_base();
    if (g_err) return;
    if (addrv.is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
    long addr = (long)addrv.num;
    if (tok != T_EQ) { err("Expected '='"); return; }
    lex_next();
    if (op == T_DOLLAR) {
        value_t s = need_str();
        if (g_err) return;
        mem_write_str(addr, s.str, s.len);
    } else {
        double v = need_num();
        if (g_err) return;
        if (op == T_QUERY) mem_pokeb(addr, (long)v); else mem_pokew(addr, (long)v);
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

// COLOUR n            : text colour (0..7 foreground, 128..135 background)
// COLOUR l, r, g, b   : redefine logical colour l's palette entry to an RGB value
static void stmt_colour(void) {
    lex_next();                              // consume COLOUR
    int l = (int)need_num();
    if (g_err) return;
    if (tok == T_COMMA) {                     // palette redefinition
        lex_next();
        int r = (int)need_num(); if (!expect(T_COMMA)) return;
        int g = (int)need_num(); if (!expect(T_COMMA)) return;
        int b = (int)need_num(); if (g_err) return;
        con_palette(l, r, g, b);
        return;
    }
    con_colour(l);                           // BBC COLOUR n (foreground 0..7)
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

// --- EXIT / CONTINUE : break out of / restart the innermost loop -------------
// Is source position (l1,o1) strictly later than (l2,o2)?
static int pos_gt(int l1, int o1, int l2, int o2) {
    return (l1 > l2) || (l1 == l2 && o1 > o2);
}

// From the current lexer position, scan forward for the `need`-th loop terminator
// (NEXT/UNTIL/ENDWHILE) at nesting depth 0. If `past`, resume just after that
// terminator's whole statement (EXIT leaves the loop); otherwise resume AT the
// terminator so it executes (CONTINUE runs the loop's test). Sets g_branch.
static void loop_break_scan(int need, int past) {
    int pc = cur_line_idx;
    int depth = 0;
    for (;;) {
        while (tok != T_EOL) {
            if (tok == T_KW && (tok_kw == KW_FOR || tok_kw == KW_REPEAT || tok_kw == KW_WHILE))
                depth++;
            else if (tok == T_KW && (tok_kw == KW_NEXT || tok_kw == KW_UNTIL || tok_kw == KW_ENDWHILE)) {
                if (depth > 0) depth--;
                else if (--need == 0) {
                    if (!past) {                 // CONTINUE: land on the terminator
                        g_branch = 1; g_branch_line = pc; g_branch_off = (int)(tok_start - cur_text);
                        return;
                    }
                    int ck = tok_kw; lex_next();     // EXIT: step past the terminator statement
                    if (ck == KW_NEXT) { if (tok == T_VAR) lex_next(); }        // NEXT [var]
                    else if (ck == KW_UNTIL) { while (tok != T_EOL && tok != T_COLON) lex_next(); }
                    g_branch = 1; g_branch_line = pc; g_branch_off = (int)(tok_start - cur_text);
                    return;
                }
            }
            lex_next();
        }
        if (++pc >= prog_n) { err("Loop is not closed (missing NEXT/UNTIL/ENDWHILE)"); return; }
        lex_at(pc, 0);
    }
}

// Shared EXIT/CONTINUE core. is_exit: leave the loop (pop its frame, resume after
// its terminator); else CONTINUE (keep the frame, resume at the terminator's test).
// want: 0 = innermost loop of any kind, else LOOP_FOR/REPEAT/WHILE.
static void loop_control(int is_exit, int want) {
    const char *what = is_exit ? "EXIT" : "CONTINUE";
    if (for_sp + repeat_sp + while_sp == 0) { err2(what, " is not inside a loop"); return; }

    int t_type, t_line, t_off;
    if (want == LOOP_FOR) {
        if (for_sp <= 0) { err2(what, " FOR: no FOR loop is active"); return; }
        t_type = LOOP_FOR; t_line = for_stack[for_sp-1].line; t_off = for_stack[for_sp-1].off;
    } else if (want == LOOP_REPEAT) {
        if (repeat_sp <= 0) { err2(what, " REPEAT: no REPEAT loop is active"); return; }
        t_type = LOOP_REPEAT; t_line = repeat_stack[repeat_sp-1].line; t_off = repeat_stack[repeat_sp-1].off;
    } else if (want == LOOP_WHILE) {
        if (while_sp <= 0) { err2(what, " WHILE: no WHILE loop is active"); return; }
        t_type = LOOP_WHILE; t_line = while_stack[while_sp-1].line; t_off = while_stack[while_sp-1].off;
    } else {                                    // innermost loop = latest start position
        t_type = 0; t_line = -1; t_off = -1;
        if (for_sp > 0 && pos_gt(for_stack[for_sp-1].line, for_stack[for_sp-1].off, t_line, t_off)) {
            t_type = LOOP_FOR; t_line = for_stack[for_sp-1].line; t_off = for_stack[for_sp-1].off; }
        if (repeat_sp > 0 && pos_gt(repeat_stack[repeat_sp-1].line, repeat_stack[repeat_sp-1].off, t_line, t_off)) {
            t_type = LOOP_REPEAT; t_line = repeat_stack[repeat_sp-1].line; t_off = repeat_stack[repeat_sp-1].off; }
        if (while_sp > 0 && pos_gt(while_stack[while_sp-1].line, while_stack[while_sp-1].off, t_line, t_off)) {
            t_type = LOOP_WHILE; t_line = while_stack[while_sp-1].line; t_off = while_stack[while_sp-1].off; }
    }

    // Pop every loop nested inside the target (a strictly later start position).
    int inner = 0;
    while (for_sp > 0    && pos_gt(for_stack[for_sp-1].line,       for_stack[for_sp-1].off,       t_line, t_off)) { for_sp--;    inner++; }
    while (repeat_sp > 0 && pos_gt(repeat_stack[repeat_sp-1].line, repeat_stack[repeat_sp-1].off, t_line, t_off)) { repeat_sp--; inner++; }
    while (while_sp > 0  && pos_gt(while_stack[while_sp-1].line,   while_stack[while_sp-1].off,   t_line, t_off)) { while_sp--;  inner++; }
    if (is_exit) {                              // EXIT also discards the target itself
        if      (t_type == LOOP_FOR)    for_sp--;
        else if (t_type == LOOP_REPEAT) repeat_sp--;
        else                            while_sp--;
    }
    loop_break_scan(inner + 1, is_exit);
}

// EXIT [FOR|REPEAT|WHILE] : leave the innermost (or named) loop.
static void stmt_exit(void) {
    lex_next();                                 // consume EXIT
    int want = 0;
    if (tok == T_KW && tok_kw == KW_FOR)         { want = LOOP_FOR;    lex_next(); }
    else if (tok == T_KW && tok_kw == KW_REPEAT) { want = LOOP_REPEAT; lex_next(); }
    else if (tok == T_KW && tok_kw == KW_WHILE)  { want = LOOP_WHILE;  lex_next(); }
    loop_control(1, want);
}

// CONTINUE [FOR|REPEAT|WHILE] : jump to the innermost (or named) loop's next test.
static void stmt_continue(void) {
    lex_next();                                 // consume CONTINUE
    int want = 0;
    if (tok == T_KW && tok_kw == KW_FOR)         { want = LOOP_FOR;    lex_next(); }
    else if (tok == T_KW && tok_kw == KW_REPEAT) { want = LOOP_REPEAT; lex_next(); }
    else if (tok == T_KW && tok_kw == KW_WHILE)  { want = LOOP_WHILE;  lex_next(); }
    loop_control(0, want);
}

static void stmt_dim(void) {
    lex_next();                              // consume DIM
    for (;;) {
        if (tok != T_VAR) { err("Expected a variable name"); return; }
        char name[NAME_LEN];
        s_copy(name, tok_var, NAME_LEN);
        int isstr = name_is_str(name);
        lex_next();

        if (tok != T_LP) {                   // DIM name <size>: reserve a byte block
            if (isstr) { err("Reserve memory into a numeric variable"); return; }
            double szd = need_num();         // DIM P% 100 reserves 101 bytes (0..100)
            if (g_err) return;
            long base = dim_reserve((int)szd + 1);
            if (g_err) return;
            var_t *v = var_find(name);
            if (v) v->num = trunc_int(v->is_int, (double)base);
            if (tok == T_COMMA) { lex_next(); continue; }
            break;
        }

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
    if (tok == T_HASH) { stmt_input_file(); return; }   // INPUT# ch, var, ...
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

// MOUSE x, y, b : read the pointer into three numeric variables at once.
// x/y are raw framebuffer pixels (origin top-left); b is the button bitmask
// (bit0=left, bit1=right, bit2=middle). See also the MOUSEX/MOUSEY/MOUSEB
// functions for reading a single value inside an expression.
static void stmt_mouse(void) {
    lex_next();                              // consume MOUSE
    int px, py, pb;
    con_mouse(&px, &py, &pb);
    int vals[3] = { px, py, pb };
    for (int i = 0; i < 3; i++) {
        if (tok != T_VAR) { err("Expected a variable name"); return; }
        char name[NAME_LEN];
        s_copy(name, tok_var, NAME_LEN);
        lex_next();
        var_t *var = var_find(name);
        if (!var) return;
        if (var->is_str) { err("MOUSE needs numeric variables"); return; }
        var->num = (double)vals[i];
        if (i < 2 && !expect(T_COMMA)) return;
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
        if (prog[i].module != 0) continue;                         // never list module lines
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
            int m = cur_module();                // DATA is scoped to the reader's module
            while (data_pc < prog_n && prog[data_pc].module == m &&
                   !line_is_data(prog[data_pc].text, &body)) data_pc++;
            if (data_pc >= prog_n || prog[data_pc].module != m) return 0;
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

// ---------------------------------------------------------------------------
// Event handlers: ON TIMER / ON PIN / ON MOUSE register a PROC to run when
// something happens, instead of busy-polling. Events are dispatched *between
// statements* by poll_events() (see run_program / run_body), never from a real
// interrupt, so the interpreter is never re-entered at an unsafe point. A handler
// is an ordinary parameterless PROC; while one runs, further events are held off.
// ---------------------------------------------------------------------------
#define EV_PIN_MAX 8

static struct { int active; char proc[NAME_LEN];
                long interval_us; unsigned long long last_us; } ev_timer;
static struct { int active; char proc[NAME_LEN]; int x, y, b; } ev_mouse;
static struct { int active; char proc[NAME_LEN]; } ev_key;
static struct { int active; char proc[NAME_LEN]; int pin, edge, level; } ev_pin[EV_PIN_MAX];
static int  in_event;                            // reentrancy guard while a handler runs
static unsigned long long g_frame_us;            // WAIT's ~60 Hz cadence clock

static void events_reset(void) {
    ev_timer.active = 0;
    ev_mouse.active = 0;
    ev_key.active = 0;
    for (int i = 0; i < EV_PIN_MAX; i++) {
        if (ev_pin[i].active) gpio_disarm_edge(ev_pin[i].pin);
        ev_pin[i].active = 0;
    }
    in_event = 0;
    g_frame_us = 0;
    g_pending_key = -1;
    con_backbuffer(0);                           // every program starts drawing to the screen
    con_sprite_tint(0, 0, 0, 0, 0);                 // ... and untinted
    con_target_screen();                         // ... and not redirected into a sprite
}

// Read the handler's PROC name (glued "PROCname" or spaced "PROC name") into out.
static int read_handler_proc(char *out) {
    if (tok != T_KW || tok_kw != KW_PROC) { err("Expected PROC and a handler name"); return 0; }
    if (tok_var[0]) { s_copy(out, tok_var, NAME_LEN); lex_next(); return 1; }
    lex_next();
    if (tok != T_VAR) { err("Expected PROC and a handler name"); return 0; }
    s_copy(out, tok_var, NAME_LEN); lex_next();
    return 1;
}
static int word_is(const char *w) { return tok == T_VAR && s_eq(tok_var, w); }

// ON TIMER cs PROC name   |   ON TIMER OFF
static void on_timer(void) {
    lex_next();                                  // consume TIMER
    if (word_is("OFF")) { lex_next(); ev_timer.active = 0; return; }
    long cs = (long)need_num(); if (g_err) return;
    if (cs < 1) cs = 1;
    char nm[NAME_LEN]; if (!read_handler_proc(nm)) return;
    s_copy(ev_timer.proc, nm, NAME_LEN);
    ev_timer.interval_us = cs * 10000L;
    ev_timer.last_us = con_micros();
    ev_timer.active = 1;
}

// ON MOUSE PROC name   |   ON MOUSE OFF
static void on_mouse(void) {
    lex_next();                                  // consume MOUSE
    if (word_is("OFF")) { lex_next(); ev_mouse.active = 0; return; }
    char nm[NAME_LEN]; if (!read_handler_proc(nm)) return;
    s_copy(ev_mouse.proc, nm, NAME_LEN);
    con_mouse(&ev_mouse.x, &ev_mouse.y, &ev_mouse.b);   // baseline to compare against
    ev_mouse.active = 1;
}

// ON KEY PROC name   |   ON KEY OFF
// The handler reads the triggering key with GET / GET$ / INKEY(0), which return
// the very key that fired the event (it is held in g_pending_key).
static void on_key(void) {
    lex_next();                                  // consume KEY
    if (word_is("OFF")) { lex_next(); ev_key.active = 0; return; }
    char nm[NAME_LEN]; if (!read_handler_proc(nm)) return;
    s_copy(ev_key.proc, nm, NAME_LEN);
    ev_key.active = 1;
}

// ON PIN p [RISING|FALLING] PROC name   |   ON PIN p OFF   |   ON PIN OFF
static void on_pin(void) {
    lex_next();                                  // consume PIN
    if (word_is("OFF")) { lex_next();            // cancel every pin handler
        for (int i = 0; i < EV_PIN_MAX; i++)
            if (ev_pin[i].active) { gpio_disarm_edge(ev_pin[i].pin); ev_pin[i].active = 0; }
        return;
    }
    if (!gpio_available()) { err("GPIO needs the Pi, not the host build"); return; }
    int p = (int)need_num(); if (g_err) return;
    if (p < 0 || p > 27) { err("No such pin"); return; }
    if (word_is("OFF")) { lex_next();            // cancel just this pin
        for (int i = 0; i < EV_PIN_MAX; i++)
            if (ev_pin[i].active && ev_pin[i].pin == p) { gpio_disarm_edge(p); ev_pin[i].active = 0; }
        return;
    }
    int edge = 2;                                // default: both edges
    if      (word_is("RISING"))  { edge = 1; lex_next(); }
    else if (word_is("FALLING")) { edge = 0; lex_next(); }
    char nm[NAME_LEN]; if (!read_handler_proc(nm)) return;
    int slot = -1;
    for (int i = 0; i < EV_PIN_MAX; i++) if (ev_pin[i].active && ev_pin[i].pin == p) slot = i;
    if (slot < 0) for (int i = 0; i < EV_PIN_MAX; i++) if (!ev_pin[i].active) { slot = i; break; }
    if (slot < 0) { err("Too many pin handlers"); return; }
    s_copy(ev_pin[slot].proc, nm, NAME_LEN);
    ev_pin[slot].pin = p; ev_pin[slot].edge = edge; ev_pin[slot].active = 1;
    ev_pin[slot].level = gpio_read(p);           // baseline for software change detect
    gpio_arm_edge(p, edge);
}

// WAIT : block until the next ~1/60 s frame boundary, giving an animation loop a
// steady cadence without a busy spin in the program. (There is no hardware vsync
// interrupt on this path; this is frame pacing, not tear-free page flipping.)
static void stmt_wait(void) {
    lex_next();
    const unsigned long long period = 16667;     // ~60 Hz
    unsigned long long now = con_micros();
    if (g_frame_us == 0 || now > g_frame_us + period) g_frame_us = now;   // (re)sync if idle/behind
    g_frame_us += period;
    while (con_micros() < g_frame_us) sound_pump();   // keep background audio going
}

// DELAY cs : pause for cs centiseconds (the same units as TIME and INKEY), so
// DELAY 50 waits half a second. Unlike an empty counting loop, the duration is
// real-time and independent of CPU speed. Background audio keeps playing during
// the wait; a non-positive value returns at once.
static void stmt_delay(void) {
    lex_next();                                       // consume DELAY
    double cs = need_num();
    if (g_err || cs <= 0) return;
    unsigned long long target = con_micros() + (unsigned long long)(cs * 10000.0);
    while (con_micros() < target) sound_pump();
}

static void stmt_on(void) {
    lex_next();                                  // consume ON
    if (tok == T_KW && tok_kw == KW_PIN)   { on_pin();   return; }
    if (tok == T_KW && tok_kw == KW_MOUSE) { on_mouse(); return; }
    if (word_is("TIMER"))                  { on_timer(); return; }
    if (word_is("KEY"))                    { on_key();   return; }
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
        case STG_EEXIST:    err("Already exists");                 break;
        case STG_ENOTEMPTY: err("Directory not empty");            break;
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
        if (prog[i].module != 0) continue;                         // never save module lines
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

// ---------------------------------------------------------------------------
// Modules (IMPORT). A module is an ordinary numbered BASIC text file holding
// DEF PROC / DEF fn definitions. At RUN, every `IMPORT "name"` line in the main
// program (and, transitively, in imported modules) loads that file and appends
// its lines to prog[] under a fresh module id. Line numbers may overlap the main
// program's because line-number lookups are scoped per module (see cur_module).
// The appended lines are stripped when the run ends, so editing/LIST/SAVE only
// ever see the main program.
// ---------------------------------------------------------------------------
#define MAX_MODULES 16
static char imported_names[MAX_MODULES][64];
static int  n_imported;

// Append one line to `module`'s block, keeping that block sorted by line number.
// Module blocks sit at the tail of prog[] (after main and earlier modules).
static void module_add_line(int module, int num, const char *text) {
    if (prog_n >= MAX_LINES) { err("Out of memory"); return; }
    int i = prog_n;
    while (i > 0 && prog[i-1].module == module && prog[i-1].num > num) {
        prog[i] = prog[i-1]; i--;
    }
    prog[i].num = num; prog[i].module = module;
    s_copy(prog[i].text, text, LINE_LEN);
    prog_n++;
}

// Load a module file and append its numbered lines under `module`. Returns 0, or
// a negative STG_* error if the file could not be read.
static int load_module(const char *name, int module) {
    int n = stg_read(name, stg_buf, STG_BUF_SIZE);
    if (n < 0) return n;
    int i = 0;
    while (i < n && !g_err) {
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
            module_add_line(module, num, p);
        }
    }
    return 0;
}

// If `text` begins with IMPORT "name", copy the (.BAS-defaulted) file name into
// `out` and return 1; otherwise 0. Uses the global lexer, safe in the pre-pass.
static int line_import_name(const char *text, char *out, int outsz) {
    cur_text = text; lx = text; lex_next();
    if (!(tok == T_KW && tok_kw == KW_IMPORT)) return 0;
    return read_filename(out, outsz);          // reads from lx, applies the .BAS default
}

// Pre-pass, run before a program executes: pull in every module reachable via
// IMPORT and append its lines. prog_n grows as modules are added, so the loop
// also visits IMPORT lines inside modules (transitive). Dedup by name breaks
// cycles and avoids importing the same module twice.
static void import_modules(void) {
    n_imported = 0;
    for (int i = 0; i < prog_n && !g_err; i++) {
        char name[64];
        if (!line_import_name(prog[i].text, name, sizeof name)) {
            if (g_err) return;                 // malformed IMPORT (no name)
            continue;
        }
        int dup = 0;
        for (int k = 0; k < n_imported; k++)
            if (s_eq(imported_names[k], name)) { dup = 1; break; }
        if (dup) continue;
        if (n_imported >= MAX_MODULES) { err("Too many imported modules"); return; }
        s_copy(imported_names[n_imported], name, 64);
        if (load_module(name, n_imported + 1) < 0) { err("Imported module not found"); return; }
        n_imported++;
    }
}

static void stmt_delete(void) {
    char name[64];
    if (!read_filename(name, sizeof name)) return;
    int r = stg_delete(name);
    if (r) stg_err(r);
}

// Directory commands take a quoted path (no ".BAS" default): MKDIR/CD/RMDIR
// "name", and PWD prints the current directory.
static void stmt_mkdir(void) {
    lex_next();
    value_t s = need_str(); if (g_err) return;
    char nm[64]; copy_fname(s, nm, sizeof nm);
    int r = stg_mkdir(nm); if (r) stg_err(r);
}
static void stmt_rmdir(void) {
    lex_next();
    value_t s = need_str(); if (g_err) return;
    char nm[64]; copy_fname(s, nm, sizeof nm);
    int r = stg_rmdir(nm); if (r) stg_err(r);
}
static void stmt_cd(void) {
    lex_next();
    value_t s = need_str(); if (g_err) return;
    char nm[64]; copy_fname(s, nm, sizeof nm);
    int r = stg_chdir(nm); if (r) stg_err(r);
}
static void stmt_pwd(void) {
    lex_next();
    con_puts(stg_cwd()); con_puts("\n");
}

// BPUT# ch, value : write a byte (numeric) or a whole string's bytes to a channel.
static void stmt_bput(void) {
    lex_next();                              // consume BPUT
    int ch = read_channel();
    if (g_err) return;
    if (!expect(T_COMMA)) return;
    value_t v = eval_expr();
    if (g_err) return;
    if (v.is_str) {                          // BPUT# ch, A$ writes the bytes verbatim
        for (int i = 0; i < v.len; i++)
            if (stg_putb(ch, (unsigned char)v.str[i]) < 0) { err("File write error"); return; }
    } else {
        if (stg_putb(ch, (int)v.num & 0xFF) < 0) { err("File write error"); return; }
    }
}

// CLOSE# ch : close one channel; CLOSE# 0 closes every open channel.
static void stmt_close(void) {
    lex_next();                              // consume CLOSE
    int ch = read_channel();
    if (g_err) return;
    if (ch == 0) stg_close_all();
    else         stg_close(ch);
}

// SEED h%, "FILE.SED" : load a native seed from storage into an executable slot
// and put its handle (1..SEED_MAX) into the numeric variable h%.
static void stmt_seed(void) {
    lex_next();                                  // consume SEED
    if (tok != T_VAR) { err("Expected a variable name"); return; }
    char hname[NAME_LEN];
    s_copy(hname, tok_var, NAME_LEN);
    if (name_is_str(hname)) { err("Seed handle must be a numeric variable"); return; }
    lex_next();
    if (!expect(T_COMMA)) return;
    if (tok != T_STR) { err("Expected a file name"); return; }

    char name[64];
    int n = 0;
    while (tok_str[n] && n < (int)sizeof(name) - 1) { name[n] = tok_str[n]; n++; }
    name[n] = 0;
    lex_next();
    int has_dot = 0;                             // default the extension to .SED
    for (int i = 0; i < n; i++) if (name[i] == '.') has_dot = 1;
    if (!has_dot && n + 4 < (int)sizeof(name)) {
        name[n++] = '.'; name[n++] = 'S'; name[n++] = 'E'; name[n++] = 'D'; name[n] = 0;
    }

    int len = stg_read(name, stg_buf, STG_BUF_SIZE);
    if (len < 0) { stg_err(len); return; }
    if (len < (int)sizeof(struct seed_header)) { err("Not a valid seed file"); return; }

    struct seed_header hdr;                       // copy bytewise (-mstrict-align safe)
    for (int i = 0; i < (int)sizeof(hdr); i++) ((char *)&hdr)[i] = stg_buf[i];
    if (hdr.magic != SEED_MAGIC)            { err("Not a valid seed file"); return; }
    if (hdr.version > SEED_ABI_VERSION)     { err("Seed needs a newer interpreter"); return; }
    if (len > SEED_SLOT_SIZE)               { err("Seed is too big"); return; }
    if (hdr.entry_off >= (uint32_t)len)     { err("Not a valid seed file"); return; }

    int slot = -1;
    for (int i = 0; i < SEED_MAX; i++) if (!seed_loaded[i]) { slot = i; break; }
    if (slot < 0) { err("Too many seeds loaded"); return; }

    for (int i = 0; i < SEED_SLOT_SIZE; i++) seed_pool[slot][i] = 0;   // clears .bss
    for (int i = 0; i < len; i++) seed_pool[slot][i] = stg_buf[i];
    icache_sync(seed_pool[slot], (unsigned long)len);                  // make executable
    seed_entry_ptr[slot] = (seed_entry)(void *)(seed_pool[slot] + hdr.entry_off);
    seed_loaded[slot] = 1;

    var_t *v = var_find(hname);
    if (v) v->num = trunc_int(v->is_int, (double)(slot + 1));
}

// CALL h%, arg... : invoke a seed for its side effects, discarding the result.
// (CALL(...) and CALL$(...) as functions return the numeric / string result.)
static void stmt_call(void) {
    lex_next();                                  // consume CALL
    int paren = (tok == T_LP);
    if (paren) lex_next();
    (void)seed_run_collect();
    if (g_err) return;
    if (paren) expect(T_RP);
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

// --- Sound: a portable, queued, background tone player ----------------------
// The hardware backend (sound.h) plays at most one square-wave tone at a time.
// Everything that makes it feel like BBC SOUND — four independent channels, a
// per-channel note queue, note durations, and "play in the background while the
// program keeps running" — lives here, so it behaves identically on the target
// and the host and is exercised by the unit tests.
//
// Each channel advances on its own clock: at every sound_pump() a channel whose
// current note has expired starts the next queued note. All channels can be
// "sounding" at once, but with a single mono voice only the lowest-numbered
// active channel is actually audible; the rest still count down so they don't
// starve. sound_pump() must be called often (it is: from the run loop, the REPL,
// and the target's key-wait idle loop) — but because the tone is sustained in
// hardware, the current note keeps playing even between pumps.

#define SND_CHANS 4          // BBC channels 0..3
#define SND_QLEN  9          // per-channel ring buffer (8 usable notes)

typedef struct { int freq, vol; long dur_us; } snd_note;   // dur_us < 0 = play forever

static struct {
    snd_note q[SND_QLEN];
    int head, tail;                        // ring buffer indices
    int active;                            // a note is currently sounding
    int forever;                           // the current note has no end time
    int cur_freq, cur_vol;
    unsigned long long end_us;             // when the current note ends
} snd_ch[SND_CHANS];

static int snd_out_freq = -1, snd_out_vol = -1;   // last tone handed to the hardware

static void sound_reset(void) {
    for (int c = 0; c < SND_CHANS; c++) {
        snd_ch[c].head = snd_ch[c].tail = 0;
        snd_ch[c].active = snd_ch[c].forever = 0;
        snd_ch[c].cur_freq = snd_ch[c].cur_vol = 0;
        snd_ch[c].end_us = 0;
    }
    snd_out_freq = snd_out_vol = -1;
    snd_silence();
}

static void sound_enqueue(int chan, int freq, int vol, long dur_us) {
    if (chan < 0 || chan >= SND_CHANS) return;
    int nt = (snd_ch[chan].tail + 1) % SND_QLEN;
    if (nt == snd_ch[chan].head) return;               // queue full: drop the note
    snd_ch[chan].q[snd_ch[chan].tail].freq   = freq;
    snd_ch[chan].q[snd_ch[chan].tail].vol    = vol;
    snd_ch[chan].q[snd_ch[chan].tail].dur_us = dur_us;
    snd_ch[chan].tail = nt;
}

// Advance every channel's playback and push the audible tone to the hardware.
// Public so the backends' idle loops can keep the queue moving.
void sound_pump(void) {
    unsigned long long now = con_micros();
    for (int c = 0; c < SND_CHANS; c++) {
        if (snd_ch[c].active && !snd_ch[c].forever && now >= snd_ch[c].end_us)
            snd_ch[c].active = 0;                        // this note finished
        if (!snd_ch[c].active && snd_ch[c].head != snd_ch[c].tail) {
            snd_note n = snd_ch[c].q[snd_ch[c].head];
            snd_ch[c].head = (snd_ch[c].head + 1) % SND_QLEN;
            snd_ch[c].active   = 1;
            snd_ch[c].forever  = (n.dur_us < 0);
            snd_ch[c].cur_freq = n.freq;
            snd_ch[c].cur_vol  = n.vol;
            snd_ch[c].end_us   = now + (unsigned long long)(n.dur_us < 0 ? 0 : n.dur_us);
        }
    }
    int wf = 0, wv = 0;
    for (int c = 0; c < SND_CHANS; c++)                  // lowest active channel wins
        if (snd_ch[c].active) { wf = snd_ch[c].cur_freq; wv = snd_ch[c].cur_vol; break; }
    if (wf != snd_out_freq || wv != snd_out_vol) {       // only touch hardware on a change
        snd_out_freq = wf; snd_out_vol = wv;
        if (wf > 0 && wv > 0) snd_set_tone(wf, wv);
        else                  snd_silence();
    }
}

// Test / introspection helpers (also drive the host `basic_host` build).
int sound_cur_freq(void) { return snd_out_freq > 0 ? snd_out_freq : 0; }
int sound_cur_vol(void)  { return snd_out_vol  > 0 ? snd_out_vol  : 0; }
int sound_queued(void) {
    int n = 0;
    for (int c = 0; c < SND_CHANS; c++) {
        int cnt = snd_ch[c].tail - snd_ch[c].head;
        if (cnt < 0) cnt += SND_QLEN;
        n += cnt + (snd_ch[c].active ? 1 : 0);
    }
    return n;
}

// BBC pitch -> Hz: 4 units = 1 semitone (48 = 1 octave), and pitch 89 is the A
// above middle C (440 Hz), so freq = 440 * 2^((pitch-89)/48).
static int pitch_to_hz(int pitch) {
    return (int)(440.0 * dpow(2.0, (pitch - 89) / 48.0) + 0.5);
}

// SOUND channel, amplitude, pitch, duration   (BBC-style)
//   channel   0..3
//   amplitude 0 = silent, else loudness; -15..-1 (BBC) and 1..15 both map to 1..15
//   pitch     0..255, quarter-semitone steps (53 = middle C, 89 = A 440)
//   duration  in twentieths of a second; -1 = play until replaced
// SOUND OFF   silences everything and empties the queues.
// --- GPIO statements --------------------------------------------------------
// Numbering is BCM: PIN 17 is BCM GPIO 17, not header pin 17. Valid pins are the
// 40-pin header, BCM 0..27 (avoid 0/1, the HAT ID EEPROM); higher pins drive the
// SD card and system peripherals and are deliberately unreachable. Every verb
// first checks that we are on real hardware (the host build has no header).

// Raise the host guard; returns 1 if GPIO is unavailable (caller should bail).
static int gpio_guard(void) {
    if (!gpio_available()) { err("GPIO needs the Pi, not the host build"); return 1; }
    return 0;
}
static int gpio_pin_arg(void) {          // read+validate a BCM header pin (0..27)
    int p = (int)need_num();
    if (g_err) return -1;
    if (p < 0 || p > 27) { err("No such pin"); return -1; }
    return p;
}

// PINMODE pin, OUTPUT | INPUT [PULLUP|PULLDOWN] | ALT f
static void stmt_pinmode(void) {
    lex_next();                          // consume PINMODE
    if (gpio_guard()) return;
    int pin = gpio_pin_arg(); if (g_err) return;
    if (!expect(T_COMMA)) return;
    if (tok != T_KW) { err("Bad pin mode"); return; }
    int m = tok_kw;
    if (m == KW_OUTPUT) {
        lex_next();
        gpio_set_mode(pin, GPIO_OUT, 0);
        gpio_set_pull(pin, GPIO_PULL_NONE);
    } else if (m == KW_INPUT) {
        lex_next();
        int pull = GPIO_PULL_NONE;
        if (tok == T_KW && tok_kw == KW_PULLUP)        { pull = GPIO_PULL_UP;   lex_next(); }
        else if (tok == T_KW && tok_kw == KW_PULLDOWN) { pull = GPIO_PULL_DOWN; lex_next(); }
        gpio_set_mode(pin, GPIO_IN, 0);
        gpio_set_pull(pin, pull);
    } else if (m == KW_ALT) {
        lex_next();
        int f = (int)need_num(); if (g_err) return;
        if (f < 0 || f > 5) { err("Bad pin mode"); return; }
        gpio_set_mode(pin, GPIO_ALT, f);
    } else {
        err("Bad pin mode");
    }
}

// PIN pin, level     (statement form; the read form PIN(n) is in eval_function)
static void stmt_pin(void) {
    lex_next();                          // consume PIN
    if (gpio_guard()) return;
    int pin = gpio_pin_arg(); if (g_err) return;
    if (!expect(T_COMMA)) return;
    int v = (int)need_num(); if (g_err) return;
    gpio_write(pin, v != 0);
}

// PINSET mask / PINCLR mask : atomically set/clear every pin whose bit is 1.
static void stmt_pinset(int setit) {
    lex_next();                          // consume PINSET/PINCLR
    if (gpio_guard()) return;
    uint32_t mask = (uint32_t)(long)need_num(); if (g_err) return;
    mask &= 0x0FFFFFFFu;
    if (setit) gpio_set_mask(mask); else gpio_clr_mask(mask);
}

// I2C needs a real Pi (QEMU does not model the BSC); raise a clear error otherwise.
static int i2c_guard(void) {
    if (!i2c_available()) { err("I2C needs real Pi hardware"); return 1; }
    return 0;
}

// I2CWRITE addr, b1 [, b2, ...] : send the listed bytes to the device at `addr`.
static void stmt_i2cwrite(void) {
    lex_next();                          // consume I2CWRITE
    if (i2c_guard()) return;
    int addr = (int)need_num(); if (!expect(T_COMMA)) return;
    unsigned char buf[64]; int n = 0;
    for (;;) {
        int v = (int)need_num(); if (g_err) return;
        if (n >= (int)sizeof buf) { err("Too many bytes"); return; }
        buf[n++] = (unsigned char)v;
        if (tok != T_COMMA) break;
        lex_next();
    }
    if (i2c_write(addr, buf, n) < 0) err("I2C write failed");
}

// I2CREAD addr, buf, count : read `count` bytes from `addr` into the DIM buffer.
static void stmt_i2cread(void) {
    lex_next();                          // consume I2CREAD
    if (i2c_guard()) return;
    int addr = (int)need_num(); if (!expect(T_COMMA)) return;
    long ad = (long)need_num(); if (!expect(T_COMMA)) return;
    int n = (int)need_num(); if (g_err) return;
    if (n <= 0 || n > 65535) { err("Invalid argument"); return; }
    // The driver writes the buffer one byte at a time, so an unaligned DIM
    // address is safe (-mstrict-align); read straight into it.
    if (i2c_read(addr, (unsigned char *)(uintptr_t)ad, n) < 0) err("I2C read failed");
}

static void stmt_sound(void) {
    lex_next();                                  // consume SOUND
    if (tok == T_VAR && s_eq(tok_var, "OFF")) { lex_next(); sound_reset(); return; }
    int chan = (int)need_num();   if (!expect(T_COMMA)) return;
    int amp  = (int)need_num();   if (!expect(T_COMMA)) return;
    int pit  = (int)need_num();   if (!expect(T_COMMA)) return;
    int dur  = (int)need_num();   if (g_err) return;
    int vol = amp < 0 ? -amp : amp;              // accept BBC's -15..0 and a plain 0..15
    if (vol > 15) vol = 15;
    long dur_us = dur < 0 ? -1L : (long)dur * 50000L;    // a "twentieth" = 50 ms
    sound_enqueue(chan, pitch_to_hz(pit), vol, dur_us);
}

// TONE frequency_hz, duration_ms [, volume]   (direct, non-BBC helper)
// Plays on channel 0. Volume defaults to full (15).
static void stmt_tone(void) {
    lex_next();                                  // consume TONE
    int freq = (int)need_num();   if (!expect(T_COMMA)) return;
    int ms   = (int)need_num();
    int vol  = 15;
    if (tok == T_COMMA) { lex_next(); vol = (int)need_num(); if (vol > 15) vol = 15; }
    if (g_err) return;
    long dur_us = ms < 0 ? -1L : (long)ms * 1000L;
    sound_enqueue(0, freq, vol, dur_us);
}

// SCREEN width, height : switch the display resolution for this program.
// SCREEN (no arguments) restores the startup resolution. The system also restores
// it automatically when the program finishes (see run_program). The BBC logical
// coordinate system (0..1279 x 0..1023) is unchanged, so graphics keep working;
// only pixel density and the text grid change. Read the current size with the
// SCREENW / SCREENH functions.
static void stmt_screen(void) {
    lex_next();                                  // consume SCREEN
    if (tok == T_EOL || tok == T_COLON) {        // bare SCREEN -> restore startup
        con_screen(0, 0);
        return;
    }
    int w = (int)need_num();   if (!expect(T_COMMA)) return;
    int h = (int)need_num();   if (g_err) return;
    if (!con_screen(w, h)) err("Could not set that screen resolution");
}

// KEYBOARD "NO" : select the keyboard layout by two-letter code (US/UK/NO/DK/SE/
// DE, case-insensitive). Affects how physical key presses are decoded from here
// on; the current layout can be read back with the KEYBOARD$ function.
static void stmt_keyboard(void) {
    lex_next();                                  // consume KEYBOARD
    value_t s = need_str();   if (g_err) return;
    char code[8];
    int n = s.len; if (n > (int)sizeof code - 1) n = (int)sizeof code - 1;
    for (int i = 0; i < n; i++) code[i] = s.str[i];
    code[n] = 0;
    if (!con_set_keyboard(code)) err("Unknown keyboard layout");
}

// POKE addr, byte : store one byte at a memory address. Familiar alias for the
// indirection form `?addr = byte`; use `!addr = word` / `$addr = "..."` for a
// 32-bit word or a string. On this bare-metal machine the address is real, so
// poke inside a DIMed buffer (see DIM name size) unless you mean a hardware one.
static void stmt_poke_kw(void) {
    lex_next();                                  // consume POKE
    long a = (long)need_num();   if (!expect(T_COMMA)) return;
    long v = (long)need_num();   if (g_err) return;
    mem_pokeb(a, v);
}

// EXEC "statement" : run a string as if it were a line of BASIC, in the current
// context (same variables, arrays, open loops). The companion to EVAL: EVAL
// computes a value, EXEC performs actions. Build the string however you like —
// a config file line, an assembled "LET " + n$ + " = " + v$, a typed command.
// The lexer is pointed at a private copy of the string and restored afterwards,
// so the statement that issued EXEC carries on normally (e.g. EXEC c$ : PRINT).
// A branch (GOTO/GOSUB) or END inside the string propagates out as usual; a
// half-open block (a FOR with no NEXT, say) is a mistake, because the copied
// text is gone once EXEC returns.
static void stmt_exec(void) {
    lex_next();                                  // consume EXEC
    value_t s = need_str();   if (g_err) return;
    char buf[LINE_LEN];
    int n = s.len; if (n > LINE_LEN - 1) n = LINE_LEN - 1;
    for (int i = 0; i < n; i++) buf[i] = s.str[i];
    buf[n] = 0;
    lexstate_t save; lex_save(&save);
    exec_text(buf, 0);
    lex_restore(&save);
}

// --- Graphics statements ----------------------------------------------------
static void stmt_mode(void) {
    lex_next();                                  // consume MODE
    int n = (int)need_num();
    if (g_err) return;
    con_mode(n);
}

// A packed colour from RGB() carries bit 30, so it can't be mistaken for a
// logical colour index (0..15 / 128..143).
#define RGB_TAG 0x40000000
static void gcol_apply_packed(int packed) {      // truecolour foreground
    con_gcol_rgb((packed >> 16) & 255, (packed >> 8) & 255, packed & 255);
}

// GCOL colour            (action 0)
// GCOL action, colour    (colour = logical index, or a packed RGB() value)
// GCOL r, g, b           (24-bit truecolour foreground)
static void stmt_gcol(void) {
    lex_next();                                  // consume GCOL
    int a = (int)need_num(); if (g_err) return;
    if (tok != T_COMMA) {                        // GCOL c
        if (a & RGB_TAG) gcol_apply_packed(a);
        else             con_gcol(0, a);
        return;
    }
    lex_next();
    int b = (int)need_num(); if (g_err) return;
    if (tok != T_COMMA) {                         // GCOL action, colour
        if (b & RGB_TAG) { con_gcol(a, 0); gcol_apply_packed(b); }
        else             con_gcol(a, b);
        return;
    }
    lex_next();
    int c = (int)need_num(); if (g_err) return;   // GCOL r, g, b
    con_gcol_rgb(a, b, c);
}

// Read `n` more comma-separated numbers into out[]. Returns 0 on error.
static int read_nums(int *out, int n) {
    for (int i = 0; i < n; i++) {
        if (i && !expect(T_COMMA)) return 0;
        out[i] = (int)need_num();
        if (g_err) return 0;
    }
    return 1;
}

// LINE x1,y1,x2,y2
static void stmt_line(void) {
    lex_next(); int v[4];
    if (!read_nums(v, 4)) return;
    con_line(v[0], v[1], v[2], v[3]);
}

// After a shape keyword, an optional FILL modifier means the solid variant.
static int shape_fill(void) {
    if (tok == T_KW && tok_kw == KW_FILL) { lex_next(); return 1; }
    return 0;
}

// RECTANGLE [FILL] x,y,w,h
static void stmt_rectangle(void) {
    lex_next(); int f = shape_fill(); int v[4];
    if (!read_nums(v, 4)) return;
    con_rectangle(v[0], v[1], v[2], v[3], f);
}

// CIRCLE [FILL] x,y,r
static void stmt_circle(void) {
    lex_next(); int f = shape_fill(); int v[3];
    if (!read_nums(v, 3)) return;
    con_circle(v[0], v[1], v[2], f);
}

// ELLIPSE [FILL] x,y,rx,ry
static void stmt_ellipse(void) {
    lex_next(); int f = shape_fill(); int v[4];
    if (!read_nums(v, 4)) return;
    con_ellipse(v[0], v[1], v[2], v[3], f);
}

// FILL x,y : flood fill from a point
static void stmt_fill(void) {
    lex_next(); int v[2];
    if (!read_nums(v, 2)) return;
    con_fill(v[0], v[1]);
}

// GGET addr, x1,y1,x2,y2 : capture a screen rectangle into a DIM buffer
static void stmt_gget(void) {
    lex_next();
    long addr = (long)need_num(); if (!expect(T_COMMA)) return;
    int v[4];
    if (!read_nums(v, 4)) return;
    con_sprite_get(addr, v[0], v[1], v[2], v[3]);
}

// GPUT addr,x,y [,scale,angle] : stamp a sprite, optionally scaled and rotated.
static void stmt_gput(void) {
    lex_next();
    long addr = (long)need_num(); if (!expect(T_COMMA)) return;
    int x = (int)need_num(); if (!expect(T_COMMA)) return;
    int y = (int)need_num();
    if (tok == T_COMMA) {                         // extended form: , scale , angle
        lex_next();
        double sc = need_num(); if (!expect(T_COMMA)) return;
        double an = need_num(); if (g_err) return;
        con_sprite_put_ex(addr, x, y, sc, an);
        return;
    }
    if (g_err) return;
    con_sprite_put(addr, x, y);                    // plain fast path
}

// GTINT r,g,b,a | GTINT OFF : tint every subsequently blitted sprite.
static void stmt_gtint(void) {
    lex_next();                                   // consume GTINT
    if (word_is("OFF")) { lex_next(); con_sprite_tint(0, 0, 0, 0, 0); return; }
    int v[4];
    if (!read_nums(v, 4)) return;                  // r,g,b,a
    con_sprite_tint(1, v[0], v[1], v[2], v[3]);
}

// NEWSPRITE addr, w, h : initialise a DIM buffer as a blank transparent sprite.
static void stmt_newsprite(void) {
    lex_next();
    long addr = (long)need_num(); if (!expect(T_COMMA)) return;
    int wh[2];
    if (!read_nums(wh, 2)) return;                 // w, h
    if (wh[0] <= 0 || wh[1] <= 0) { err("Bad sprite size"); return; }
    con_newsprite(addr, wh[0], wh[1]);
}

// SPRITETARGET addr | SPRITETARGET OFF : redirect drawing into a sprite (or back).
static void stmt_spritetarget(void) {
    lex_next();
    if (word_is("OFF")) { lex_next(); con_target_screen(); return; }
    long addr = (long)need_num(); if (g_err) return;
    if (con_target_sprite(addr) < 0) err("Bad sprite");
}

// TILEMAP sheet,map,cols,rows,tilew,tileh,scrollx,scrolly : draw a tile grid.
static void stmt_tilemap(void) {
    lex_next();
    long sheet = (long)need_num(); if (!expect(T_COMMA)) return;
    long map   = (long)need_num(); if (!expect(T_COMMA)) return;
    int v[6];                                      // cols,rows,tilew,tileh,scrollx,scrolly
    if (!read_nums(v, 6)) return;
    con_tilemap(sheet, map, v[0], v[1], v[2], v[3], v[4], v[5]);
}

// --- Collection statements --------------------------------------------------
// Copy a just-read string key into a stable local buffer: the value expression
// that follows may run the GC and move the key's bytes in the string heap.
static int grab_key(value_t k, char *buf) {
    int n = k.len; if (n > MAX_STR) n = MAX_STR;
    for (int i = 0; i < n; i++) buf[i] = k.str[i];
    return n;
}

// DICTSET d, key$, value   |   DICTDEL d, key$
static void stmt_dictset(void) {
    lex_next();
    double h = need_num(); if (!expect(T_COMMA)) return;
    value_t k = need_str(); if (g_err) return;
    char key[MAX_STR]; int klen = grab_key(k, key);
    if (!expect(T_COMMA)) return;
    value_t v = eval_expr(); if (g_err) return;
    dict_t *D = (dict_t *)coll_get(h, CT_DICT); if (!D) return;
    dict_set(D, key, klen, v);
}
static void stmt_dictdel(void) {
    lex_next();
    double h = need_num(); if (!expect(T_COMMA)) return;
    value_t k = need_str(); if (g_err) return;
    char key[MAX_STR]; int klen = grab_key(k, key);
    dict_t *D = (dict_t *)coll_get(h, CT_DICT); if (!D) return;
    dict_del(D, key, klen);
}

// PUSH L, value  |  LISTSET L, i, value  |  LISTINS L, i, value  |  LISTDEL L, i
static void stmt_push(void) {
    lex_next();
    double h = need_num(); if (!expect(T_COMMA)) return;
    value_t v = eval_expr(); if (g_err) return;
    list_t *L = (list_t *)coll_get(h, CT_LIST); if (!L) return;
    list_ins(L, L->len, v);                        // append
}
static void stmt_listset(void) {
    lex_next();
    double h = need_num(); if (!expect(T_COMMA)) return;
    int i = (int)need_num(); if (!expect(T_COMMA)) return;
    value_t v = eval_expr(); if (g_err) return;
    list_t *L = (list_t *)coll_get(h, CT_LIST); if (!L) return;
    if (i < 0 || i >= L->len) { err("Index out of range"); return; }
    cval_store(&L->item[i], v);
}
static void stmt_listins(void) {
    lex_next();
    double h = need_num(); if (!expect(T_COMMA)) return;
    int i = (int)need_num(); if (!expect(T_COMMA)) return;
    value_t v = eval_expr(); if (g_err) return;
    list_t *L = (list_t *)coll_get(h, CT_LIST); if (!L) return;
    list_ins(L, i, v);
}
static void stmt_listdel(void) {
    lex_next();
    double h = need_num(); if (!expect(T_COMMA)) return;
    int i = (int)need_num(); if (g_err) return;
    list_t *L = (list_t *)coll_get(h, CT_LIST); if (!L) return;
    list_del(L, i);
}

// TREESET t, key, value   |   TREEDEL t, key
static void stmt_treeset(void) {
    lex_next();
    double h = need_num(); if (!expect(T_COMMA)) return;
    double key = need_num(); if (!expect(T_COMMA)) return;
    value_t v = eval_expr(); if (g_err) return;
    tree_t *T = (tree_t *)coll_get(h, CT_TREE); if (!T) return;
    tree_set(T, key, v);
}
static void stmt_treedel(void) {
    lex_next();
    double h = need_num(); if (!expect(T_COMMA)) return;
    double key = need_num(); if (g_err) return;
    tree_t *T = (tree_t *)coll_get(h, CT_TREE); if (!T) return;
    tree_del(T, key);
}

// BUFFER ON | BUFFER OFF : enable/disable off-screen drawing (double buffering).
// While on, every draw targets the back buffer and the screen freezes until FLIP.
static void stmt_buffer(void) {
    lex_next();                                   // consume BUFFER
    if (tok == T_KW && tok_kw == KW_ON) {
        lex_next();
        if (con_backbuffer(1) < 0) err("Could not allocate the back buffer");
    } else if (word_is("OFF")) {
        lex_next();
        con_backbuffer(0);
    } else {
        err("Expected ON or OFF");
    }
}

// FLIP : present the back buffer to the screen (a no-op when buffering is off).
static void stmt_flip(void) {
    lex_next();                                   // consume FLIP
    con_flip();
}

// SAVESPRITE addr, "file" : write a sprite (LOADSPRITE result or GGET capture)
// out as an image file (PNG, or BMP if the name ends in .bmp).
static void stmt_savesprite(void) {
    lex_next();
    long addr = (long)need_num(); if (!expect(T_COMMA)) return;
    value_t s = need_str(); if (g_err) return;
    char nm[64]; copy_fname(s, nm, sizeof nm);
    if (img_save_sprite(nm, addr) != 0) err("Could not save sprite");
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

// --- TRY / CATCH / ENDTRY : structured error handling -----------------------
// Forward-scan from just after TRY for the matching CATCH (honouring nested
// TRY blocks). Records its position in *pc/*off (the start of the handler body).
// Returns 1 on success; raises and returns 0 if there is no CATCH.
static int find_catch(int *out_pc, int *out_off) {
    int pc = cur_line_idx;
    int depth = 0;
    for (;;) {
        while (tok != T_EOL) {
            if (tok == T_KW && tok_kw == KW_TRY) depth++;
            else if (tok == T_KW && tok_kw == KW_ENDTRY) {
                if (depth == 0) { err("TRY without a matching CATCH"); return 0; }
                depth--;
            } else if (tok == T_KW && tok_kw == KW_CATCH && depth == 0) {
                lex_next();                          // step past CATCH
                *out_pc = pc; *out_off = (int)(tok_start - cur_text);
                return 1;
            }
            lex_next();
        }
        if (++pc >= prog_n) { err("TRY without a matching CATCH"); return 0; }
        lex_at(pc, 0);
    }
}

// TRY : begin a protected block. Locate its CATCH, snapshot the interpreter
// state, push a handler, then carry on executing the block.
static void stmt_try(void) {
    lex_next();                                  // consume TRY
    if (cur_line_idx < 0) { err("TRY can only be used inside a program"); return; }
    if (try_sp >= TRY_MAX) { err("Too many nested TRY blocks"); return; }

    // Remember where the token after TRY starts, so we can resume the block after
    // the forward scan for CATCH has moved the lexer around.
    const char *save_text = cur_text;
    int save_line = cur_line_idx;
    int save_off  = (int)(tok_start - cur_text);

    int cpc, coff;
    if (!find_catch(&cpc, &coff)) return;

    // Re-lex the token right after TRY so the block continues cleanly.
    cur_text = save_text; cur_line_idx = save_line;
    lx = cur_text + save_off;
    lex_next();

    try_rec_t *h = &try_stack[try_sp++];
    h->catch_pc = cpc; h->catch_off = coff;
    h->call_sp = call_sp;
    h->for_sp = for_sp; h->repeat_sp = repeat_sp; h->while_sp = while_sp;
    h->case_sp = case_sp; h->gosub_sp = gosub_sp; h->fn_ret_sp = fn_ret_sp;
    h->local_sp = local_sp;
    h->scratch_base = scratch_base; h->scratch_top = scratch_top;
}

// CATCH reached in normal flow: the protected block finished with no error, so
// discard the handler and skip past the matching ENDTRY (don't run the handler).
static void stmt_catch(void) {
    if (try_sp > 0) try_sp--;                    // this block succeeded
    lex_next();                                  // consume CATCH
    skip_to_close(KW_TRY, KW_ENDTRY, "CATCH without a matching ENDTRY");
}

static void stmt_endtry(void) { lex_next(); }    // no-op join point

// RAISE : throw a user error.
//   RAISE "message"            (code 0)
//   RAISE code                 (numeric code, message = "")
//   RAISE code, "message"
static void stmt_raise(void) {
    lex_next();                                  // consume RAISE
    value_t v = eval_expr();
    if (g_err) return;
    int code = 0;
    char msgbuf[ERRMSG_MAX]; msgbuf[0] = 0;
    if (v.is_str) {
        int n = v.len < ERRMSG_MAX - 1 ? v.len : ERRMSG_MAX - 1;
        for (int i = 0; i < n; i++) msgbuf[i] = v.str[i];
        msgbuf[n] = 0;
    } else {
        code = (int)v.num;
        if (tok == T_COMMA) {
            lex_next();
            value_t m = need_str();
            if (g_err) return;
            int n = m.len < ERRMSG_MAX - 1 ? m.len : ERRMSG_MAX - 1;
            for (int i = 0; i < n; i++) msgbuf[i] = m.str[i];
            msgbuf[n] = 0;
        }
    }
    if (g_err) return;
    // Raise it the same way err() does, but keep the caller-supplied code.
    err(msgbuf[0] ? msgbuf : "User error");
    g_errcode = code;
}

// If an error is pending and the innermost TRY handler belongs to the current
// PROC/FN frame, unwind to it: restore the saved stacks/locals, clear the error,
// and report the CATCH position to resume at (via *pc/*off). Returns 1 if caught.
static int try_handle_error(int *pc, int *off) {
    if (!g_err || try_sp <= 0) return 0;
    try_rec_t *h = &try_stack[try_sp - 1];
    if (h->call_sp != call_sp) return 0;         // handler is in an outer frame: keep unwinding

    for_sp = h->for_sp; repeat_sp = h->repeat_sp; while_sp = h->while_sp;
    case_sp = h->case_sp; gosub_sp = h->gosub_sp; fn_ret_sp = h->fn_ret_sp;
    while (local_sp > h->local_sp) {             // undo LOCALs bound inside the block
        local_sp--;
        *local_stack[local_sp].slot = local_stack[local_sp].old;
    }
    scratch_base = h->scratch_base; scratch_top = h->scratch_top;

    *pc = h->catch_pc; *off = h->catch_off;
    try_sp--;                                    // this handler is now consumed
    g_err = 0;                                   // handled: resume normal flow
    g_branch = 0; g_return = 0;
    return 1;
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
            case KW_EXIT:     stmt_exit();     return;
            case KW_CONTINUE: stmt_continue(); return;
            case KW_TRY:      stmt_try();      return;
            case KW_CATCH:    stmt_catch();    return;
            case KW_ENDTRY:   stmt_endtry();   return;
            case KW_RAISE:    stmt_raise();    return;
            case KW_ENDIF: lex_next();     return;   // block-IF join point: no-op
            case KW_ELSE:  stmt_else_block(); return;   // end of a block-IF THEN branch
            case KW_CLS:   lex_next(); con_cls();   return;
            case KW_COLOUR:stmt_colour();  return;
            case KW_SOUND: stmt_sound();   return;
            case KW_TONE:  stmt_tone();    return;
            case KW_POKE:  stmt_poke_kw(); return;
            case KW_EXEC:  stmt_exec();    return;
            case KW_PINMODE: stmt_pinmode(); return;
            case KW_PIN:     stmt_pin();     return;   // write form; read form PIN(n) in expressions
            case KW_PINSET:  stmt_pinset(1); return;
            case KW_PINCLR:  stmt_pinset(0); return;
            case KW_I2CWRITE: stmt_i2cwrite(); return;
            case KW_I2CREAD:  stmt_i2cread();  return;
            case KW_SCREEN: stmt_screen(); return;
            case KW_WAIT:   stmt_wait();   return;
            case KW_DELAY:  stmt_delay();  return;
            case KW_KEYBOARD: stmt_keyboard(); return;
            case KW_MODE:  stmt_mode();    return;
            case KW_GCOL:  stmt_gcol();    return;
            case KW_PLOT:  stmt_plot();    return;
            case KW_MOVE:  stmt_move_draw(4); return;   // MOVE = PLOT 4
            case KW_DRAW:  stmt_move_draw(5); return;   // DRAW = PLOT 5
            case KW_CLG:   lex_next(); con_clg();   return;
            case KW_LINE:      stmt_line();      return;
            case KW_RECTANGLE: stmt_rectangle(); return;
            case KW_CIRCLE:    stmt_circle();    return;
            case KW_ELLIPSE:   stmt_ellipse();   return;
            case KW_FILL:      stmt_fill();      return;
            case KW_GGET:      stmt_gget();      return;
            case KW_GPUT:      stmt_gput();      return;
            case KW_BUFFER:    stmt_buffer();    return;
            case KW_FLIP:      stmt_flip();      return;
            case KW_GTINT:     stmt_gtint();     return;
            case KW_NEWSPRITE:    stmt_newsprite();    return;
            case KW_SPRITETARGET: stmt_spritetarget(); return;
            case KW_TILEMAP:      stmt_tilemap();      return;
            case KW_DICTSET:  stmt_dictset();  return;
            case KW_DICTDEL:  stmt_dictdel();  return;
            case KW_PUSH:     stmt_push();     return;
            case KW_LISTSET:  stmt_listset();  return;
            case KW_LISTINS:  stmt_listins();  return;
            case KW_LISTDEL:  stmt_listdel();  return;
            case KW_TREESET:  stmt_treeset();  return;
            case KW_TREEDEL:  stmt_treedel();  return;
            case KW_SAVESPRITE: stmt_savesprite(); return;
            case KW_SAVE:  stmt_save();    return;
            case KW_LOAD:  stmt_load();    return;
            case KW_DELETE:stmt_delete();  return;
            case KW_SEED:  stmt_seed();    return;
            case KW_CALL:  stmt_call();    return;
            case KW_CAT:
            case KW_DIR:   lex_next(); stg_dir(); return;
            case KW_MKDIR: stmt_mkdir(); return;
            case KW_CD:    stmt_cd();    return;
            case KW_RMDIR: stmt_rmdir(); return;
            case KW_PWD:   stmt_pwd();   return;
            case KW_PROC:  call_proc(0, 0); return;
            case KW_ENDPROC: g_return = 1; return;
            case KW_LOCAL: stmt_local();   return;
            case KW_DEF:   tok = T_EOL;    return;   // DEF lines are skipped in normal flow
            case KW_IMPORT: tok = T_EOL;   return;   // resolved in the pre-pass; a no-op here
            case KW_REM:   tok = T_EOL;    return;   // ignore rest of line
            case KW_END:                             // END fn/proc = return; bare END = stop
                lex_next();
                if (tok == T_KW && (tok_kw == KW_FN || tok_kw == KW_PROC)) {
                    g_return = 1; return;
                }
                g_stop = 1; return;
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
            case KW_MOUSE:    stmt_mouse();    return;
            case KW_BPUT:     stmt_bput();     return;
            case KW_CLOSE:    stmt_close();    return;
            case KW_PTR: {                             // PTR# ch = expr : set file pointer
                lex_next();                            // consume PTR
                int ch = read_channel();
                if (g_err) return;
                if (tok != T_EQ) { err("Expected '='"); return; }
                lex_next();
                long p = (long)need_num();
                if (g_err) return;
                stg_seek(ch, p);
                return;
            }
            case KW_LIST:     stmt_list();     return;
            case KW_RENUMBER: stmt_renumber(); return;
            case KW_AUTO:     stmt_auto();     return;
            case KW_EDIT:     stmt_edit();     return;
            case KW_NEW:   lex_next(); prog_n = 0; clear_vars();
                           for (int i = 0; i < SEED_MAX; i++) seed_loaded[i] = 0;
                           seed_heap_reset(); coll_reset();
                           for_sp = 0; gosub_sp = 0; return;
            default:       err("That keyword can't be used as a command");  return;
        }
    }
    if (tok == T_VAR) { stmt_let(0); return; }   // implicit assignment
    if (tok == T_QUERY || tok == T_PLING || tok == T_DOLLAR) { stmt_poke(); return; }
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
            int is_fn = (tok_kw == KW_FN);
            int newstyle = 0;
            if (tok_var[0] == 0) {                // spaced: "DEF fn NAME" / "DEF proc NAME"
                newstyle = 1;
                lex_next();                       // the name follows as its own word
                if (tok != T_VAR) continue;       // malformed DEF line
            }
            s_copy(defs[def_n].name, tok_var, NAME_LEN);
            defs[def_n].is_fn    = is_fn;
            defs[def_n].newstyle = newstyle;
            defs[def_n].line     = i;
            defs[def_n].off      = cur_off();     // just after the name ('(' or body)
            def_n++;
        }
    }
}

// Run a registered event handler PROC (parameterless). call_named parses its
// argument list from the current token, so force T_EOL first to bind zero args;
// call_named saves and restores the rest of the caller's lexer state itself.
static void dispatch_handler(const char *name) {
    tok = T_EOL;
    call_named(0, name, 0);
}

// Check every armed event source and run its handler if it has fired. Called at
// statement/line boundaries from the run loops. `in_event` blocks re-entry so a
// handler can't itself be interrupted by another event.
static void poll_events(void) {
    if (in_event || g_err || g_stop) return;
    if (!ev_timer.active && !ev_mouse.active && !ev_key.active) {
        int any = 0;
        for (int i = 0; i < EV_PIN_MAX; i++) if (ev_pin[i].active) { any = 1; break; }
        if (!any) return;
    }
    in_event = 1;
    if (ev_timer.active) {
        unsigned long long now = con_micros();
        if ((long long)(now - ev_timer.last_us) >= ev_timer.interval_us) {
            ev_timer.last_us = now;
            dispatch_handler(ev_timer.proc);
        }
    }
    for (int i = 0; i < EV_PIN_MAX && !g_err && !g_stop; i++) {
        if (!ev_pin[i].active) continue;
        // Fire on a hardware-latched edge (catches transient pulses on real
        // hardware) OR a software level change of the requested direction (works
        // everywhere, including QEMU which doesn't model the edge detector).
        int cur = gpio_read(ev_pin[i].pin);
        int latched = gpio_poll_edge(ev_pin[i].pin);
        int e = ev_pin[i].edge, prev = ev_pin[i].level;
        int fire = latched
                || (e == 2 && cur != prev)
                || (e == 1 && prev == 0 && cur == 1)
                || (e == 0 && prev == 1 && cur == 0);
        ev_pin[i].level = cur;
        if (fire) dispatch_handler(ev_pin[i].proc);
    }
    if (ev_mouse.active && !g_err && !g_stop) {
        int x, y, b; con_mouse(&x, &y, &b);
        if (x != ev_mouse.x || y != ev_mouse.y || b != ev_mouse.b) {
            ev_mouse.x = x; ev_mouse.y = y; ev_mouse.b = b;
            dispatch_handler(ev_mouse.proc);
        }
    }
    // A keypress: consume it and hold it in g_pending_key so the handler's GET /
    // INKEY reads the same key. Skip if a key is already waiting to be read.
    if (ev_key.active && !g_err && !g_stop && g_pending_key < 0) {
        int k = con_inkey(0);
        if (k >= 0) { g_pending_key = k; dispatch_handler(ev_key.proc); }
    }
    in_event = 0;
}

// Execute program lines starting at (pc,off) until ENDPROC / =<expr> (g_return),
// END (g_stop), end of program, or an error. Used for PROC and FN bodies.
static void run_body(int pc, int off) {
    g_return = 0;
    while (pc >= 0 && pc < prog_n && !g_err && !g_stop && !g_return) {
        poll_events();                           // fire any events between lines
        if (g_err || g_stop || g_return) break;
        cur_line_idx = pc;
        g_runline = prog[pc].num;
        g_branch = 0;
        exec_text(prog[pc].text, off);
        off = 0;
        if (g_err) {                             // a TRY in this frame can catch it
            int npc, noff;
            if (try_handle_error(&npc, &noff)) { pc = npc; off = noff; continue; }
        }
        if (g_err || g_stop || g_return) break;
        if (g_branch) { pc = g_branch_line; off = g_branch_off; }
        else          { pc++; }
    }
}

static int run_depth;                          // nested run_program depth (for IMPORT)

static void run_program(int start_pc, int start_off) {
    int outer = (run_depth == 0);
    run_depth++;
    // Only the outermost RUN pulls in modules (and strips them at the end), so a
    // program that itself issues RUN doesn't re-import on top of the loaded set.
    if (outer) { main_n = prog_n; import_modules(); }
    g_stop = 0;
    gosub_sp = 0;
    for_sp = 0;
    repeat_sp = 0;
    while_sp = 0;
    case_sp = 0;
    call_sp = 0;
    fn_ret_sp = 0;
    local_sp = 0;
    try_sp = 0;
    g_errcode = 0; g_errmsg[0] = 0;
    scratch_base = 0;
    for (int i = 0; i < SEED_MAX; i++) seed_loaded[i] = 0;   // fresh run reloads its seeds
    seed_heap_reset();                                       // and starts with a clean heap
    coll_reset();                                            // ... including its collections
    sound_reset();                                           // silence any leftover notes
    if (outer) events_reset();                               // cancel any ON TIMER/PIN/MOUSE
    data_pc = 0;
    data_off = -1;
    scan_defs();                                            // now also sees module PROC/FN
    int pc  = start_pc;
    int off = start_off;
    // Top-level flow runs the MAIN program only (indices [0, main_n)); module
    // lines live above main_n and are reached solely through PROC/FN calls, so
    // falling off the end of main must not wander into them.
    while (pc >= 0 && pc < main_n && !g_err && !g_stop) {
        sound_pump();                            // advance any queued/background notes
        poll_events();                           // fire ON TIMER/PIN/MOUSE handlers
        if (g_err || g_stop) break;
        cur_line_idx = pc;
        g_runline = prog[pc].num;
        g_branch = 0;
        g_return = 0;
        exec_text(prog[pc].text, off);
        off = 0;
        if (g_err) {                             // a TRY at the top level can catch it
            int npc, noff;
            if (try_handle_error(&npc, &noff)) { pc = npc; off = noff; continue; }
        }
        if (g_err || g_stop) break;
        if (g_branch) { pc = g_branch_line; off = g_branch_off; }
        else          { pc++; }
    }
    cur_line_idx = -1;
    g_runline = -1;
    g_stop = 0;
    g_branch = 0;
    run_depth--;
    if (outer) {
        // An error deferred by an unmatched TRY (or one that never reached its
        // CATCH) was recorded but not printed; report it now so it isn't lost.
        if (g_err && !g_err_reported) {
            con_putc('\n'); con_puts(g_errmsg);
            g_runline = g_errline; err_tail(); g_runline = -1;
            g_err_reported = 1;
        }
        try_sp = 0;
        prog_n = main_n;                       // strip imported module lines
        con_screen(0, 0);                      // restore the startup resolution (no-op if unchanged)
        con_backbuffer(0);                     // un-buffer so the prompt draws to the visible screen
    }
}

// ---------------------------------------------------------------------------
// PROC / FN call. `name` is the callee; on entry the current token is at the
// argument list ('(' or, for a no-arg call, whatever follows the name).
// Evaluates arguments, binds them (plus any LOCALs) over the callee's
// parameters, runs the body, then restores the caller's state. A function
// returns its value in *retval: an old-style FN via =<expr>, a new-style
// `DEF fn NAME` via the value of its NAME variable at END fn.
// ---------------------------------------------------------------------------

#define MAX_ARGS 16

static void call_named(int is_fn, const char *name, value_t *retval) {
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
    int save_try = try_sp;                       // discard any TRY the body leaves open

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

    // New-style functions return the value of a variable named after the
    // function. Bind it as a fresh local (so it is private and restored on
    // exit) and remember it so END fn / the post-body read can find it.
    int pushed_ret = 0;
    if (ok && is_fn && defs[d].newstyle) {
        if (fn_ret_sp >= FN_RET_MAX) { err("Too many nested function calls"); ok = 0; }
        else {
            var_t *rv = var_find(name);
            if (!rv) ok = 0;
            else {
                local_stack[local_sp].slot = rv;
                local_stack[local_sp].old  = *rv;
                local_sp++;
                rv->is_str = name_is_str(name);
                rv->num = 0; rv->s.sptr = 0; rv->s.slen = 0;
                fn_ret_slot[fn_ret_sp++] = rv;
                pushed_ret = 1;
            }
        }
    }

    if (ok) {
        run_body(defs[d].line, body_off);
        if (is_fn && defs[d].newstyle && !g_err) {    // return = the NAME variable
            var_t *rv = fn_ret_slot[fn_ret_sp - 1];
            fn_retval = rv->is_str ? str_in_scratch(rv->s.sptr, rv->s.slen)
                                   : v_num(rv->num);
        }
        if (is_fn && retval && !g_err) *retval = fn_retval;
    }
    if (pushed_ret) fn_ret_sp--;

    // Restore locals (parameters + any LOCALs declared in the body).
    while (local_sp > local_mark) {
        local_sp--;
        *local_stack[local_sp].slot = local_stack[local_sp].old;
    }

    // Restore the caller's state. Keep scratch_top for FN so the return value
    // (which lives in scratch) survives; rewind it for PROC.
    call_sp--;
    try_sp = save_try;                           // drop TRY handlers opened in the body
    scratch_base = save_sbase;
    if (!is_fn) scratch_top = save_stop;
    g_return = 0;
    lx = save_lx; cur_text = save_text;
    tok = save_tok; tok_kw = save_kw; tok_num = save_num;
    s_copy(tok_var, save_var, NAME_LEN);
    cur_line_idx = save_line; g_runline = save_runline;
    g_branch = save_br; g_branch_line = save_brl; g_branch_off = save_bro;
}

// PROC / FN call written with the keyword prefix (PROCname / FNname). The current
// token is KW_PROC or KW_FN with the name in tok_var; consume it, then delegate.
static void call_proc(int is_fn, value_t *retval) {
    char name[NAME_LEN];
    s_copy(name, tok_var, NAME_LEN);
    lex_next();                                  // consume the PROC/FN token
    call_named(is_fn, name, retval);
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
    main_n = 0;
    run_depth = 0;
    n_imported = 0;
    var_n = 0;
    g_err = 0;
    g_runline = -1;
    cur_line_idx = -1;
    g_stop = 0;
    g_branch = 0;
    gosub_sp = 0;
    for_sp = 0;
    repeat_sp = 0;
    while_sp = 0;
    case_sp = 0;
    call_sp = 0;
    local_sp = 0;
    try_sp = 0;
    g_errcode = 0;
    g_errmsg[0] = 0;
    def_n = 0;
    g_return = 0;
    scratch_top = 0;
    scratch_base = 0;
    data_pc = 0;
    data_off = -1;
    time_base = 0;
    clear_vars();
    snd_init();          // bring up the audio hardware (host backend is a no-op)
    sound_reset();
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
