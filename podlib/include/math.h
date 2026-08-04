#ifndef _POD_MATH_H
#define _POD_MATH_H
double ldexp(double x, int exp);
long double ldexpl(long double x, int e);
double frexp(double x, int *exp);
double fabs(double x);
double floor(double x);
double ceil(double x);
double pow(double x, double y);
#define HUGE_VAL (1e308*10)
#define NAN (__builtin_nanf(""))
#define INFINITY (1e308*10)
#endif
