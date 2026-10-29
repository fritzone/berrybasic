// math.c - the berry-libc <math.h>, double precision.
//
// Self-contained and freestanding: no tables of pointers, so it links into a
// seed as readily as a POD. The elementary functions use range reduction plus
// the classic fdlibm minimax polynomials, giving close to 1 ulp accuracy on the
// usual inputs; the rounding and classification helpers are exact bit work.
#include <math.h>
#include <stdint.h>

// ---- bit access -----------------------------------------------------------
typedef union { double d; uint64_t u; } bits_t;
static uint64_t D2U(double d) { bits_t b; b.d = d; return b.u; }
static double   U2D(uint64_t u) { bits_t b; b.u = u; return b.d; }

#define EXP_MASK  0x7ff0000000000000ULL
#define SIGN_MASK 0x8000000000000000ULL
#define MANT_MASK 0x000fffffffffffffULL

// ---- classification -------------------------------------------------------
int isnan(double x)    { uint64_t u = D2U(x); return (u & EXP_MASK) == EXP_MASK && (u & MANT_MASK) != 0; }
int isinf(double x)    { uint64_t u = D2U(x); return (u & EXP_MASK) == EXP_MASK && (u & MANT_MASK) == 0; }
int isfinite(double x) { return (D2U(x) & EXP_MASK) != EXP_MASK; }
int isnormal(double x) { uint64_t e = D2U(x) & EXP_MASK; return e != 0 && e != EXP_MASK; }
int signbit(double x)  { return (int)(D2U(x) >> 63); }
double copysign(double x, double y) { return U2D((D2U(x) & ~SIGN_MASK) | (D2U(y) & SIGN_MASK)); }
double fabs(double x)  { return U2D(D2U(x) & ~SIGN_MASK); }
double nan(const char *s) { (void)s; return U2D(EXP_MASK | 0x8000000000000ULL); }

double fmax(double x, double y) { if (isnan(x)) return y; if (isnan(y)) return x; return x < y ? y : x; }
double fmin(double x, double y) { if (isnan(x)) return y; if (isnan(y)) return x; return x < y ? x : y; }
double fdim(double x, double y) { if (isnan(x) || isnan(y)) return NAN; return x > y ? x - y : 0.0; }

// ---- ldexp / frexp / scalbn / modf ----------------------------------------
double ldexp(double x, int n) {
    if (x == 0.0 || !isfinite(x)) return x;
    // decompose, add n to the exponent, renormalising through the double range
    while (n > 1023) { x *= 8.98846567431158e307 /* 2^1023 */; n -= 1023; if (!isfinite(x)) return x; }
    while (n < -1022) { x *= 2.2250738585072014e-308 /* 2^-1022 */; n += 1022; if (x == 0.0) return x; }
    uint64_t u = D2U(x);
    int e = (int)((u >> 52) & 0x7ff);
    e += n;
    if (e >= 0x7ff) return copysign(INFINITY, x);
    if (e <= 0) {  // subnormal result
        return x * U2D((uint64_t)(n + 1022) << 52) * 2.2250738585072014e-308;
    }
    return U2D((u & ~EXP_MASK) | ((uint64_t)e << 52));
}
double scalbn(double x, int n) { return ldexp(x, n); }
double scalbln(double x, long n) { return ldexp(x, (int)n); }

double frexp(double x, int *e) {
    if (x == 0.0 || !isfinite(x)) { *e = 0; return x; }
    uint64_t u = D2U(x);
    int ex = (int)((u >> 52) & 0x7ff);
    if (ex == 0) { x *= 4503599627370496.0 /* 2^52 */; u = D2U(x); ex = (int)((u >> 52) & 0x7ff) - 52; }
    *e = ex - 1022;
    return U2D((u & ~EXP_MASK) | ((uint64_t)1022 << 52));   // mantissa in [0.5,1)
}

// ---- floor / ceil / trunc / round / rint ----------------------------------
double trunc(double x) {
    uint64_t u = D2U(x);
    int e = (int)((u >> 52) & 0x7ff) - 1023;
    if (e < 0) return copysign(0.0, x);
    if (e >= 52) return x;                                   // integer, inf or nan
    uint64_t m = MANT_MASK >> e;
    if ((u & m) == 0) return x;
    return U2D(u & ~m);
}
double floor(double x) { double t = trunc(x); return (t > x) ? t - 1.0 : t; }
double ceil(double x)  { double t = trunc(x); return (t < x) ? t + 1.0 : t; }
double round(double x) { double t = trunc(x); double f = x - t;
    if (f >= 0.5) return t + 1.0; if (f <= -0.5) return t - 1.0; return t; }
double rint(double x) {  // round to nearest, ties to even
    uint64_t u = D2U(x);
    int e = (int)((u >> 52) & 0x7ff) - 1023;
    if (e >= 52) return x;
    double big = copysign(4503599627370496.0 /* 2^52 */, x);
    return (x + big) - big;
}
double nearbyint(double x) { return rint(x); }
long   lround(double x) { return (long)round(x); }
long   lrint(double x)  { return (long)rint(x); }

double modf(double x, double *ip) {
    double t = trunc(x);
    *ip = t;
    if (!isfinite(x)) { return isinf(x) ? copysign(0.0, x) : x; }
    return x - t;
}

// ---- fmod / remainder -----------------------------------------------------
double fmod(double x, double y) {
    if (isnan(x) || isnan(y) || isinf(x) || y == 0.0) return NAN;
    if (isinf(y) || x == 0.0) return x;
    double ax = fabs(x), ay = fabs(y);
    if (ax < ay) return x;
    if (ax == ay) return copysign(0.0, x);
    int ex, ey; frexp(ax, &ex); frexp(ay, &ey);
    int n = ex - ey;
    double t = ldexp(ay, n);
    if (t > ax) { t *= 0.5; n--; }
    for (int i = 0; i <= n; i++) { if (ax >= t) ax -= t; t *= 0.5; }
    return copysign(ax, x);
}
double remainder(double x, double y) {
    double r = fmod(x, y);
    double ay = fabs(y);
    if (fabs(r) > 0.5 * ay) r -= copysign(ay, r);
    if (fabs(r) == 0.5 * ay) {   // ties to even
        double q = (x - r) / y;
        if (fmod(q, 2.0) != 0.0) r -= copysign(ay, r);
    }
    return r;
}
double remquo(double x, double y, int *quo) {
    double r = remainder(x, y);
    double q = (x - r) / y;
    *quo = (int)fmod(q, 8.0);
    return r;
}

// ---- sqrt / cbrt / hypot --------------------------------------------------
double sqrt(double x) {
    if (isnan(x) || x < 0.0) return (x == 0.0) ? x : NAN;   // sqrt(-0) = -0
    if (x == 0.0 || isinf(x)) return x;
    int e; double m = frexp(x, &e);                          // m in [0.5,1)
    if (e & 1) { m *= 2.0; e--; }                            // make e even
    double y = 0.41731 + 0.59016 * m;                        // linear seed on [0.5,2)
    y = 0.5 * (y + m / y);
    y = 0.5 * (y + m / y);
    y = 0.5 * (y + m / y);
    y = 0.5 * (y + m / y);
    y = y - (y * y - m) / (2.0 * y);                         // one Newton correction
    return ldexp(y, e / 2);
}
double cbrt(double x) {
    if (x == 0.0 || !isfinite(x)) return x;
    double a = fabs(x);
    int e; double m = frexp(a, &e);
    int r = e % 3; if (r < 0) r += 3;
    m = ldexp(m, r);                                          // fold remainder into mantissa
    double y = 0.5 + 0.5 * m;
    for (int i = 0; i < 6; i++) y = y - (y * y * y - m) / (3.0 * y * y);
    y = ldexp(y, (e - r) / 3);
    return copysign(y, x);
}
double hypot(double x, double y) {
    x = fabs(x); y = fabs(y);
    if (isinf(x) || isinf(y)) return INFINITY;
    if (x < y) { double t = x; x = y; y = t; }
    if (x == 0.0) return 0.0;
    double r = y / x;
    return x * sqrt(1.0 + r * r);
}

// ---- exp / log ------------------------------------------------------------
static const double LN2HI = 6.93147180369123816490e-01;
static const double LN2LO = 1.90821492927058770002e-10;
static const double LN2   = 0.69314718055994530942;
static const double INVLN2 = 1.44269504088896338700;

double exp(double x) {
    if (isnan(x)) return x;
    if (x > 709.782712893384) return INFINITY;
    if (x < -745.13321910194) return 0.0;
    int k = (int)(x * INVLN2 + (x >= 0 ? 0.5 : -0.5));
    double hi = x - k * LN2HI;
    double lo = k * LN2LO;
    double r = hi - lo;
    double t = r * r;
    // fdlibm exp core polynomial
    double c = r - t * (1.66666666666666019037e-01 + t * (-2.77777777770155933842e-03
             + t * (6.61375632143793436117e-05 + t * (-1.65339022054652515390e-06
             + t * 4.13813679705723846039e-08))));
    double y = 1.0 - ((lo - (r * c) / (2.0 - c)) - hi);
    return ldexp(y, k);
}
double exp2(double x)  { return exp(x * LN2); }
double expm1(double x) { if (fabs(x) < 1e-5) return x + 0.5 * x * x; return exp(x) - 1.0; }

double log(double x) {
    if (isnan(x) || x < 0.0) return NAN;
    if (x == 0.0) return -INFINITY;
    if (isinf(x)) return x;
    int e; double m = frexp(x, &e);                          // m in [0.5,1)
    if (m < 0.70710678118654752440) { m *= 2.0; e--; }       // fold into [sqrt(2)/2, sqrt(2))
    double s = (m - 1.0) / (m + 1.0);                         // |s| < 0.1716
    double z = s * s;
    // log(m) = 2*(s + s^3/3 + s^5/5 + ...)
    double p = 1.0 + z * (1.0/3 + z * (1.0/5 + z * (1.0/7 + z * (1.0/9
             + z * (1.0/11 + z * (1.0/13 + z * (1.0/15 + z * (1.0/17))))))));
    return e * LN2 + 2.0 * s * p;
}
double log2(double x)  { return log(x) * 1.44269504088896340736; }
double log10(double x) { return log(x) * 0.43429448190325182765; }
double log1p(double x) { if (fabs(x) < 1e-4) return x - 0.5 * x * x + x * x * x / 3.0; return log(1.0 + x); }

double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 1.0) return 1.0;
    if (isnan(x) || isnan(y)) return NAN;
    if (y == 1.0) return x;
    if (y == 2.0) return x * x;
    if (x == 0.0) return (y > 0.0) ? 0.0 : INFINITY;
    if (x > 0.0) return exp(y * log(x));
    // x < 0: only integer y gives a real result
    double ry = round(y);
    if (ry != y) return NAN;
    double mag = exp(y * log(-x));
    return (fmod(ry, 2.0) != 0.0) ? -mag : mag;
}

// ---- sin / cos / tan ------------------------------------------------------
// pi/2 split into three parts, for accurate argument reduction (fdlibm).
static const double INVPIO2 = 6.36619772367581382433e-01;
static const double PIO2_1  = 1.57079632673412561417e+00;
static const double PIO2_1T = 6.07710050650619224932e-11;
static const double PIO2_2  = 6.07710050630396597660e-11;
static const double PIO2_2T = 2.02226624879595063154e-21;
static const double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
                    S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
                    S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
static const double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
                    C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
                    C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;

static double kernel_sin(double r) {
    double z = r * r;
    return r + r * z * (S1 + z * (S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)))));
}
static double kernel_cos(double r) {
    double z = r * r;
    return 1.0 - 0.5 * z + z * z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
}
// reduce x to r in [-pi/4, pi/4] and the quadrant k mod 4. Two-step subtraction
// of the split pi/2 keeps ~1 ulp accuracy well past the small-angle range.
static int reduce_pio2(double x, double *rr) {
    double fn = rint(x * INVPIO2);
    double r = x - fn * PIO2_1;
    double w = fn * PIO2_1T;
    double t = r;
    w = fn * PIO2_2;
    r = t - w;
    w = fn * PIO2_2T - ((t - r) - w);
    *rr = r - w;
    long q = (long)fn;
    return (int)(q & 3);
}
double sin(double x) {
    if (!isfinite(x)) return NAN;
    double r; int k = reduce_pio2(x, &r);
    switch (k) {
        case 0:  return kernel_sin(r);
        case 1:  return kernel_cos(r);
        case 2:  return -kernel_sin(r);
        default: return -kernel_cos(r);
    }
}
double cos(double x) {
    if (!isfinite(x)) return NAN;
    double r; int k = reduce_pio2(x, &r);
    switch (k) {
        case 0:  return kernel_cos(r);
        case 1:  return -kernel_sin(r);
        case 2:  return -kernel_cos(r);
        default: return kernel_sin(r);
    }
}
double tan(double x) {
    if (!isfinite(x)) return NAN;
    double r; int k = reduce_pio2(x, &r);
    double s = kernel_sin(r), c = kernel_cos(r);
    return (k & 1) ? -c / s : s / c;
}

// ---- atan / asin / acos / atan2 -------------------------------------------
static double atan_reduced(double x) {   // |x| <= tan(pi/12) ~ 0.2679, Taylor
    double z = x * x;
    return x * (1.0 - z * (1.0/3 - z * (1.0/5 - z * (1.0/7 - z * (1.0/9
             - z * (1.0/11 - z * (1.0/13 - z * (1.0/15 - z * (1.0/17)))))))));
}
double atan(double x) {
    if (isnan(x)) return x;
    int neg = x < 0.0; x = fabs(x);
    if (isinf(x)) return neg ? -1.57079632679489661923 : 1.57079632679489661923;
    int inv = 0;
    if (x > 1.0) { x = 1.0 / x; inv = 1; }
    double offset = 0.0;
    if (x > 0.26794919243112270647) {                        // tan(pi/12) = 2 - sqrt(3)
        offset = 0.52359877559829887308;                     // pi/6
        x = (x * 1.73205080756887729353 - 1.0) / (x + 1.73205080756887729353);
    }
    double r = offset + atan_reduced(x);
    if (inv) r = 1.57079632679489661923 - r;
    return neg ? -r : r;
}
double atan2(double y, double x) {
    if (isnan(x) || isnan(y)) return NAN;
    if (x == 0.0 && y == 0.0) return copysign(0.0, y);       // atan2(0,+0)=0, atan2(-0,+0)=-0
    if (x == 0.0) return copysign(1.57079632679489661923, y);
    double a = atan(y / x);
    if (x > 0.0) return a;
    return (y >= 0.0) ? a + 3.14159265358979323846 : a - 3.14159265358979323846;
}
double asin(double x) {
    if (isnan(x)) return x;
    if (x > 1.0 || x < -1.0) return NAN;
    if (x == 1.0) return 1.57079632679489661923;
    if (x == -1.0) return -1.57079632679489661923;
    return atan2(x, sqrt(1.0 - x * x));
}
double acos(double x) {
    if (isnan(x)) return x;
    if (x > 1.0 || x < -1.0) return NAN;
    return atan2(sqrt(1.0 - x * x), x);
}

// ---- sinh / cosh / tanh ---------------------------------------------------
double sinh(double x) {
    if (!isfinite(x)) return x;
    if (fabs(x) < 1e-5) return x + x * x * x / 6.0;
    double e = exp(fabs(x));
    double r = 0.5 * (e - 1.0 / e);
    return copysign(r, x);
}
double cosh(double x) {
    if (isnan(x)) return x;
    double e = exp(fabs(x));
    return 0.5 * (e + 1.0 / e);
}
double tanh(double x) {
    if (isnan(x)) return x;
    if (x > 20.0) return 1.0;
    if (x < -20.0) return -1.0;
    double e = exp(2.0 * x);
    return (e - 1.0) / (e + 1.0);
}
double atanh(double x) { return 0.5 * log((1.0 + x) / (1.0 - x)); }
double asinh(double x) { return copysign(log(fabs(x) + sqrt(x * x + 1.0)), x); }
double acosh(double x) { return log(x + sqrt(x * x - 1.0)); }

// ---- float wrappers (call the double version, round the result) -----------
float sqrtf(float x)  { return (float)sqrt(x); }
float cbrtf(float x)  { return (float)cbrt(x); }
float fabsf(float x)  { return (float)fabs(x); }
float floorf(float x) { return (float)floor(x); }
float ceilf(float x)  { return (float)ceil(x); }
float truncf(float x) { return (float)trunc(x); }
float roundf(float x) { return (float)round(x); }
float sinf(float x)   { return (float)sin(x); }
float cosf(float x)   { return (float)cos(x); }
float tanf(float x)   { return (float)tan(x); }
float asinf(float x)  { return (float)asin(x); }
float acosf(float x)  { return (float)acos(x); }
float atanf(float x)  { return (float)atan(x); }
float atan2f(float y, float x) { return (float)atan2(y, x); }
float expf(float x)   { return (float)exp(x); }
float logf(float x)   { return (float)log(x); }
float log2f(float x)  { return (float)log2(x); }
float log10f(float x) { return (float)log10(x); }
float powf(float x, float y) { return (float)pow(x, y); }
float hypotf(float x, float y) { return (float)hypot(x, y); }
float fmodf(float x, float y)  { return (float)fmod(x, y); }
float sinhf(float x)  { return (float)sinh(x); }
float coshf(float x)  { return (float)cosh(x); }
float tanhf(float x)  { return (float)tanh(x); }
float ldexpf(float x, int n) { return (float)ldexp(x, n); }
float copysignf(float x, float y) { return (float)copysign(x, y); }
float fmaxf(float x, float y) { return (float)fmax(x, y); }
float fminf(float x, float y) { return (float)fmin(x, y); }
