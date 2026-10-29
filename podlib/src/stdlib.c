// <stdlib.h> for the pod-libc: allocation routes to the pod heap through the
// services table; qsort/bsearch and the string/number helpers are pure. Built
// with -fno-builtin so malloc+memset isn't folded back into a recursive calloc.
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "pod_rt.h"

// --- dynamic memory --------------------------------------------------------
void *malloc(size_t size)               { return berry_svc->alloc((unsigned)size); }
void  free(void *ptr)                   { berry_svc->free(ptr); }
void *realloc(void *ptr, size_t size)   { return berry_svc->realloc(ptr, (unsigned)size); }

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    if (size && total / size != nmemb) return NULL;     // overflow
    void *p = berry_svc->alloc((unsigned)total);
    if (p) memset(p, 0, total);
    return p;
}

// The pod heap already returns 16-byte-aligned blocks, which covers every
// natural alignment a C program needs. Larger requests over-allocate and align
// forward; the block stays freeable because a header precedes the returned ptr
// only when we did not move it (so callers must free the exact pointer). tinycc
// does not use these, so a simple best-effort is enough.
void *aligned_alloc(size_t alignment, size_t size) {
    if (alignment <= 16) return malloc(size);
    char *raw = malloc(size + alignment);
    if (!raw) return NULL;
    uintptr_t a = ((uintptr_t)raw + alignment) & ~(uintptr_t)(alignment - 1);
    return (void *)a;                                    // leaks the prefix; rare path
}
void *memalign(size_t alignment, size_t size) { return aligned_alloc(alignment, size); }

// --- qsort (heapsort: in place, O(n log n), no recursion) ------------------
static void pl_swap(char *a, char *b, size_t n) {
    while (n--) { char t = *a; *a++ = *b; *b++ = t; }
}
static void pl_siftdown(char *base, size_t i, size_t n, size_t sz,
                        int (*cmp)(const void *, const void *)) {
    for (;;) {
        size_t big = i, l = 2 * i + 1, r = 2 * i + 2;
        if (l < n && cmp(base + l * sz, base + big * sz) > 0) big = l;
        if (r < n && cmp(base + r * sz, base + big * sz) > 0) big = r;
        if (big == i) break;
        pl_swap(base + i * sz, base + big * sz, sz);
        i = big;
    }
}
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (nmemb < 2 || size == 0) return;
    char *a = base;
    for (size_t i = nmemb / 2; i-- > 0; ) pl_siftdown(a, i, nmemb, size, compar);
    for (size_t end = nmemb - 1; end > 0; end--) {
        pl_swap(a, a + end * size, size);
        pl_siftdown(a, 0, end, size, compar);
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
    const char *b = base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = compar(key, b + mid * size);
        if (c < 0) hi = mid; else if (c > 0) lo = mid + 1; else return (void *)(b + mid * size);
    }
    return NULL;
}

// --- string -> number ------------------------------------------------------
static int digit_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}
unsigned long strtoul(const char *s, char **endptr, int base) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = (*p++ == '-');
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; base = 16; }
    else if (base == 0 && p[0] == '0') base = 8;
    else if (base == 0) base = 10;
    unsigned long acc = 0; int any = 0, d;
    while ((d = digit_val((unsigned char)*p)) < base) { acc = acc * base + d; p++; any = 1; }
    if (endptr) *endptr = (char *)(any ? p : s);
    return neg ? (unsigned long)(-(long)acc) : acc;
}
long long strtoll(const char *s, char **endptr, int base) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = (*p++ == '-');
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; base = 16; }
    else if (base == 0 && p[0] == '0') base = 8;
    else if (base == 0) base = 10;
    unsigned long long acc = 0; int any = 0, d;
    while ((d = digit_val((unsigned char)*p)) < base) { acc = acc * base + d; p++; any = 1; }
    if (endptr) *endptr = (char *)(any ? p : s);
    return neg ? -(long long)acc : (long long)acc;
}
unsigned long long strtoull(const char *s, char **endptr, int base) {
    return (unsigned long long)strtoll(s, endptr, base);
}
long strtol(const char *s, char **endptr, int base)   { return (long)strtoul(s, endptr, base); }
int  atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }

int  abs(int n)   { return n < 0 ? -n : n; }
long labs(long n) { return n < 0 ? -n : n; }
long long llabs(long long n) { return n < 0 ? -n : n; }

// --- pseudo-random ---------------------------------------------------------
static unsigned long rand_state = 1;
void srand(unsigned seed) { rand_state = seed; }
int  rand(void) { rand_state = rand_state * 1103515245UL + 12345UL; return (int)((rand_state >> 16) & 0x7fffffffUL); }

// --- integer division results + a couple of allocation helpers -------------
div_t   div(int n, int d)         { div_t r;   r.quot = n / d; r.rem = n % d; return r; }
ldiv_t  ldiv(long n, long d)      { ldiv_t r;  r.quot = n / d; r.rem = n % d; return r; }
lldiv_t lldiv(long long n, long long d) { lldiv_t r; r.quot = n / d; r.rem = n % d; return r; }

void *reallocarray(void *p, size_t n, size_t sz) {
    size_t total = n * sz;
    if (sz && total / sz != n) return 0;                    // overflow
    return realloc(p, total);
}
int posix_memalign(void **memptr, size_t alignment, size_t size) {
    if (alignment < sizeof(void *) || (alignment & (alignment - 1))) return 22;   // EINVAL
    void *p = aligned_alloc(alignment, size);
    if (!p) return 12;                                       // ENOMEM
    *memptr = p;
    return 0;
}
