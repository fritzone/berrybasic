// pod-libc: the remaining odds and ends the headers promise - a few string
// searches, a minimal <math.h>, and stubs for the stat/isatty/clock surface
// that a hosted program touches but a POD does not really need.
#include <string.h>
#include <stddef.h>
#include <sys/stat.h>
#include <dirent.h>
#include "pod_rt.h"

// --- string searches -------------------------------------------------------
char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++) for (const char *a = accept; *a; a++) if (*s == *a) return (char *)s;
    return NULL;
}
size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    for (; s[n]; n++) { const char *a = accept; while (*a && *a != s[n]) a++; if (!*a) break; }
    return n;
}
size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    for (; s[n]; n++) { const char *r = reject; while (*r && *r != s[n]) r++; if (*r) break; }
    return n;
}
char *strerror(int errnum) { (void)errnum; return (char *)"error"; }

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }
int stricmp(const char *a, const char *b) {
    while (*a && lc((unsigned char)*a) == lc((unsigned char)*b)) { a++; b++; }
    return lc((unsigned char)*a) - lc((unsigned char)*b);
}
int strncasecmp(const char *a, const char *b, size_t n) {
    while (n && *a && lc((unsigned char)*a) == lc((unsigned char)*b)) { a++; b++; n--; }
    return n ? lc((unsigned char)*a) - lc((unsigned char)*b) : 0;
}
int strcasecmp(const char *a, const char *b) { return stricmp(a, b); }

// --- minimal <math.h> ------------------------------------------------------
double fabs(double x)  { return x < 0 ? -x : x; }
double floor(double x) { long long i = (long long)x; if ((double)i > x) i--; return (double)i; }
double ceil(double x)  { long long i = (long long)x; if ((double)i < x) i++; return (double)i; }
double ldexp(double x, int exp) {
    if (exp >= 0) { while (exp--) x *= 2.0; } else { while (exp++) x *= 0.5; }
    return x;
}
double frexp(double x, int *exp) {
    int e = 0;
    double a = x < 0 ? -x : x;
    if (a != 0) { while (a >= 1.0) { a *= 0.5; e++; } while (a < 0.5) { a *= 2.0; e--; } }
    if (exp) *exp = e;
    return x < 0 ? -a : a;
}
double pow(double x, double y) {          // integer exponents only (enough here)
    int n = (int)y; double r = 1.0;
    if (n < 0) { x = 1.0 / x; n = -n; }
    while (n--) r *= x;
    return r;
}
// long double (128-bit) ldexp, for tinycc's float-constant parser. The quad
// arithmetic resolves to the __*tf* soft-float helpers in lib-arm64.c.
long double ldexpl(long double x, int e) {
    if (e >= 0) while (e--) x *= 2.0L; else while (e++) x *= 0.5L;
    return x;
}

// --- stat / isatty / clock (stubs) -----------------------------------------
int stat(const char *path, struct stat *st)  { (void)path; (void)st; return -1; }
int fstat(int fd, struct stat *st)           { (void)fd; (void)st; return -1; }
int mkdir(const char *path, int mode) {       /* routes to the CAP_DIRS service */
    (void)mode;
    return (pod_svc && pod_svc->mkdir) ? pod_svc->mkdir(path) : -1;
}

// --- <dirent.h> over the CAP_DIRS dir_open/dir_read services ---------------
// The interpreter keeps a single directory-scan cursor, so there is one DIR.
struct DIR { int open; struct dirent ent; };
static struct DIR g_dir;

DIR *opendir(const char *path) {
    if (!pod_svc || !pod_svc->dir_open) return 0;
    if (!pod_svc->dir_open(path)) return 0;
    g_dir.open = 1;
    return &g_dir;
}
struct dirent *readdir(DIR *d) {
    int is_dir = 0; long size = 0;
    if (!d || !d->open || !pod_svc || !pod_svc->dir_read) return 0;
    if (!pod_svc->dir_read(d->ent.d_name, (int)sizeof d->ent.d_name, &is_dir, &size)) {
        d->open = 0;
        return 0;
    }
    d->ent.d_type = is_dir ? DT_DIR : DT_REG;
    d->ent.d_size = size;
    return &d->ent;
}
int closedir(DIR *d) { if (d) d->open = 0; return 0; }
int isatty(int fd)                    { return fd <= 2; }
long clock(void) { return pod_svc && pod_svc->time_cs ? (long)pod_svc->time_cs() : 0; }
