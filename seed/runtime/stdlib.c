// <stdlib.h> implementation for native seeds. The allocation functions route to
// the seed heap through the services pointer captured on entry; qsort/bsearch
// and the conversion/arithmetic helpers are pure. Built with -fno-builtin so
// malloc+memset isn't folded back into a recursive calloc.
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "seed.h"

// Set by the SEED_EXPORT entry trampoline before the seed body runs, so the
// allocation functions below are always valid inside a seed call.
const SeedServices *seed_svc;

// --- dynamic memory --------------------------------------------------------
void *malloc(size_t size) { return seed_svc->alloc((unsigned)size); }
void  free(void *ptr)     { seed_svc->free(ptr); }

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    if (size && total / size != nmemb) return NULL;     // overflow
    void *p = seed_svc->alloc((unsigned)total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    return seed_svc->realloc(ptr, (unsigned)size);
}

void *reallocarray(void *ptr, size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    if (size && total / size != nmemb) return NULL;     // overflow, ptr untouched
    return seed_svc->realloc(ptr, (unsigned)total);
}

void *aligned_alloc(size_t alignment, size_t size) {
    return seed_svc->alloc_aligned((unsigned)alignment, (unsigned)size);
}

void *memalign(size_t alignment, size_t size) {
    return seed_svc->alloc_aligned((unsigned)alignment, (unsigned)size);
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
        return 22;                                       // EINVAL
    void *p = seed_svc->alloc_aligned((unsigned)alignment, (unsigned)size);
    if (!p) return 12;                                   // ENOMEM
    *memptr = p;
    return 0;
}

void free_sized(void *ptr, size_t size) { (void)size; seed_svc->free(ptr); }
void free_aligned_sized(void *ptr, size_t alignment, size_t size) {
    (void)alignment; (void)size; seed_svc->free(ptr);
}

// --- qsort (heapsort: in place, O(n log n), no recursion) ------------------
static void seed_swap(char *a, char *b, size_t n) {
    while (n--) { char t = *a; *a++ = *b; *b++ = t; }
}

static void seed_siftdown(char *base, size_t i, size_t n, size_t sz,
                          int (*cmp)(const void *, const void *)) {
    for (;;) {
        size_t big = i, l = 2 * i + 1, r = 2 * i + 2;
        if (l < n && cmp(base + l * sz, base + big * sz) > 0) big = l;
        if (r < n && cmp(base + r * sz, base + big * sz) > 0) big = r;
        if (big == i) break;
        seed_swap(base + i * sz, base + big * sz, sz);
        i = big;
    }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (nmemb < 2 || size == 0) return;
    char *a = base;
    for (size_t i = nmemb / 2; i-- > 0; )                // build the max-heap
        seed_siftdown(a, i, nmemb, size, compar);
    for (size_t end = nmemb - 1; end > 0; end--) {       // pop the max repeatedly
        seed_swap(a, a + end * size, size);
        seed_siftdown(a, 0, end, size, compar);
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
    const char *b = base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = compar(key, b + mid * size);
        if (c < 0)      hi = mid;
        else if (c > 0) lo = mid + 1;
        else            return (void *)(b + mid * size);
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
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2; base = 16;
    } else if (base == 0 && p[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    unsigned long acc = 0;
    int any = 0, d;
    while ((d = digit_val((unsigned char)*p)) < base) { acc = acc * base + d; p++; any = 1; }
    if (endptr) *endptr = (char *)(any ? p : s);
    return neg ? (unsigned long)(-(long)acc) : acc;
}

long strtol(const char *s, char **endptr, int base) {
    return (long)strtoul(s, endptr, base);   // strtoul already applies the sign
}

int  atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }

// --- integer arithmetic ----------------------------------------------------
int  abs(int n)  { return n < 0 ? -n : n; }
long labs(long n) { return n < 0 ? -n : n; }

// --- pseudo-random (LCG; mirrors the interpreter's RND generator) ----------
static unsigned long rand_state = 1;
void srand(unsigned seed) { rand_state = seed; }
int  rand(void) {
    rand_state = rand_state * 1103515245UL + 12345UL;
    return (int)((rand_state >> 16) & 0x7fffffffUL);
}
