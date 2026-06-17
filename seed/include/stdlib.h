#ifndef SEED_STDLIB_H
#define SEED_STDLIB_H
// Freestanding <stdlib.h> for native seeds. Only the OS-independent subset is
// provided (the seed runtime implements it); anything needing the operating
// system - exit(), getenv(), system(), ... - is intentionally absent, and using
// it produces a clear "undefined reference" at link time. For console or BASIC
// I/O use the seed services (svc) instead.
#include <stddef.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff

// --- dynamic memory (backed by the seed heap) ------------------------------
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void *reallocarray(void *ptr, size_t nmemb, size_t size);
void  free(void *ptr);
void  free_sized(void *ptr, size_t size);                            // C23
void  free_aligned_sized(void *ptr, size_t alignment, size_t size); // C23
void *aligned_alloc(size_t alignment, size_t size);                 // C11
void *memalign(size_t alignment, size_t size);                      // legacy alias
int   posix_memalign(void **memptr, size_t alignment, size_t size);

// --- searching and sorting -------------------------------------------------
void  qsort(void *base, size_t nmemb, size_t size,
            int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

// --- string <-> number conversions -----------------------------------------
int            atoi(const char *s);
long           atol(const char *s);
long           strtol(const char *s, char **endptr, int base);
unsigned long  strtoul(const char *s, char **endptr, int base);

// --- integer arithmetic ----------------------------------------------------
int  abs(int n);
long labs(long n);

// --- pseudo-random numbers -------------------------------------------------
int  rand(void);
void srand(unsigned seed);

#endif // SEED_STDLIB_H
