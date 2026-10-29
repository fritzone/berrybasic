#ifndef _POD_MATH_H
#define _POD_MATH_H

// The berry-libc <math.h>. Double precision, with float (f-suffix) wrappers.
// Implementations are in podlib/src/math.c.

#define HUGE_VAL  (1e308 * 10)
#define INFINITY  (1e308 * 10)
#define NAN       (nan(""))

#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

// classification
int    isnan(double x);
int    isinf(double x);
int    isfinite(double x);
int    isnormal(double x);
int    signbit(double x);
double copysign(double x, double y);
double nan(const char *tag);

// basic
double fabs(double x);
double fmax(double x, double y);
double fmin(double x, double y);
double fdim(double x, double y);
double fmod(double x, double y);
double remainder(double x, double y);
double remquo(double x, double y, int *quo);

// exponent / rounding
double ldexp(double x, int n);
double scalbn(double x, int n);
double scalbln(double x, long n);
double frexp(double x, int *e);
double modf(double x, double *iptr);
double trunc(double x);
double floor(double x);
double ceil(double x);
double round(double x);
double rint(double x);
double nearbyint(double x);
long   lround(double x);
long   lrint(double x);

// power / roots
double sqrt(double x);
double cbrt(double x);
double hypot(double x, double y);
double pow(double x, double y);

// exp / log
double exp(double x);
double exp2(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);

// trigonometric
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

// hyperbolic
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);

// long double (for the compiler's float-constant parser)
long double ldexpl(long double x, int e);

// float wrappers
float sqrtf(float x);
float cbrtf(float x);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float truncf(float x);
float roundf(float x);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);
float atan2f(float y, float x);
float expf(float x);
float logf(float x);
float log2f(float x);
float log10f(float x);
float powf(float x, float y);
float hypotf(float x, float y);
float fmodf(float x, float y);
float sinhf(float x);
float coshf(float x);
float tanhf(float x);
float ldexpf(float x, int n);
float copysignf(float x, float y);
float fmaxf(float x, float y);
float fminf(float x, float y);

#endif
