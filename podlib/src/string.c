// <string.h> for the pod-libc: the freestanding string/memory routines every C
// program (and the compiler that emits POD calls to memcpy/memset/...) needs.
// Pure code; strdup/strndup pull in malloc. Built with -fno-builtin.
#include <string.h>
#include <stdlib.h>      // strdup/strndup use malloc

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = a, *q = b;
    while (n--) { if (*p != *q) return *p - *q; p++; q++; }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    while (n--) { if (*p == (unsigned char)c) return (void *)p; p++; }
    return NULL;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t i = 0;
    while (i < maxlen && s[i]) i++;
    return i;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++)) ;
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (*d) d++;
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dst;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char *)last;
    }
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *p = malloc(len + 1);
    if (p) { memcpy(p, s, len); p[len] = '\0'; }
    return p;
}

// --- more <string.h> -------------------------------------------------------
static char *g_strtok_save;
char *strtok_r(char *s, const char *delim, char **save) {
    char *p = s ? s : *save;
    if (!p) return 0;
    p += strspn(p, delim);
    if (!*p) { *save = 0; return 0; }
    char *tok = p;
    p = strpbrk(p, delim);
    if (p) { *p = 0; *save = p + 1; } else *save = 0;
    return tok;
}
char *strtok(char *s, const char *delim) { return strtok_r(s, delim, &g_strtok_save); }

void *memccpy(void *d, const void *s, int c, size_t n) {
    unsigned char *dp = d; const unsigned char *sp = s;
    while (n--) { *dp = *sp; if (*sp == (unsigned char)c) return dp + 1; dp++; sp++; }
    return 0;
}
void *memrchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s + n;
    while (n--) if (*--p == (unsigned char)c) return (void *)p;
    return 0;
}
char *strsep(char **sp, const char *delim) {
    char *s = *sp; if (!s) return 0;
    char *e = strpbrk(s, delim);
    if (e) { *e = 0; *sp = e + 1; } else *sp = 0;
    return s;
}
char *stpcpy(char *d, const char *s) { while ((*d = *s)) { d++; s++; } return d; }
char *strrev(char *s) {
    size_t i = 0, j = strlen(s); if (j) j--;
    while (i < j) { char t = s[i]; s[i++] = s[j]; s[j--] = t; }
    return s;
}
size_t strlcpy(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
size_t strlcat(char *d, const char *s, size_t n) {
    size_t dl = strnlen(d, n);
    if (dl == n) return n + strlen(s);
    size_t l = strlen(s), c = l < n - dl - 1 ? l : n - dl - 1;
    memcpy(d + dl, s, c); d[dl + c] = 0;
    return dl + l;
}
static int ci(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
char *strcasestr(const char *h, const char *n) {
    if (!*n) return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && ci((unsigned char)*a) == ci((unsigned char)*b)) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return 0;
}
