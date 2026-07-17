// ===========================================================================
// BerryBasiC — helpers, error state, floating-point math, number formatting
//
// This file is a fragment of the interpreter and is #included by basic.c.
// It is NOT a standalone translation unit: it shares the single translation
// unit's static state and is compiled only as part of basic.c. Do not add it
// to the build system or compile it on its own.
// ===========================================================================
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

