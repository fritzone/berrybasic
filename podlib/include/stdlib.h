#ifndef _POD_STDLIB_H
#define _POD_STDLIB_H
#include <stddef.h>

void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void *aligned_alloc(size_t alignment, size_t size);
void *memalign(size_t alignment, size_t size);

void  qsort(void *base, size_t nmemb, size_t size, int (*)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*)(const void *, const void *));

long          strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);
long long     strtoll(const char *s, char **endptr, int base);
unsigned long long strtoull(const char *s, char **endptr, int base);
int           atoi(const char *s);
long          atol(const char *s);
long long     atoll(const char *s);
double        strtod(const char *s, char **endptr);
float         strtof(const char *s, char **endptr);
long double   strtold(const char *s, char **endptr);
double        atof(const char *s);

int   abs(int n);
long  labs(long n);
long long llabs(long long n);

void  srand(unsigned seed);
int   rand(void);

void  exit(int code);
void  _exit(int code);
void  abort(void);
int   atexit(void (*fn)(void));
char *getenv(const char *name);
char *realpath(const char *path, char *resolved);

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff

typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;
div_t   div(int n, int d);
ldiv_t  ldiv(long n, long d);
lldiv_t lldiv(long long n, long long d);
void   *reallocarray(void *p, size_t n, size_t sz);
int     posix_memalign(void **memptr, size_t alignment, size_t size);

#endif
