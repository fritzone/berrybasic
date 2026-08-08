// pod-libc miscellany: the POSIX file layer (open/read/write/close/lseek) over
// the POD file services, plus process bits (exit/abort/getenv), time, assert,
// glob (a stub), and strtod. This is the OS surface a hosted C program (tinycc
// included) reaches for beyond stdio/stdlib/string.
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include "pod_rt.h"

// --- exit / abort (longjmp back to the crt, which returns to the loader) ----
jmp_buf __pod_exit_jmp;
int     __pod_exit_code;
static int exit_ready;

// setjmp is direct asm; longjmp normalises a 0 value to 1 here (the asm restore
// lacks a compare) and then never returns from __pod_longjmp.
extern void __pod_longjmp(jmp_buf env, int val);
void longjmp(jmp_buf env, int val) { __pod_longjmp(env, val ? val : 1); }

void __pod_exit_arm(void) { exit_ready = 1; }   // called by crt0 after setjmp

void exit(int code) {
    __pod_exit_code = code;
    if (exit_ready) longjmp(__pod_exit_jmp, 1);
    for (;;) { }                                 // no crt frame: cannot unwind
}
void _exit(int code) { exit(code); }
void abort(void) { exit(134); }
void atexit_stub(void) { }
int  atexit(void (*fn)(void)) { (void)fn; return 0; }

char *getenv(const char *name) { (void)name; return 0; }   // no environment on-device

// realpath: the card has no symlinks, so a path is already "real"; just copy it.
char *realpath(const char *path, char *resolved) {
    if (!resolved) { resolved = malloc(1024); if (!resolved) return 0; }
    int i = 0; while (path[i] && i < 1023) { resolved[i] = path[i]; i++; }
    resolved[i] = 0;
    return resolved;
}

// --- assert ----------------------------------------------------------------
void __pod_assert_fail(const char *expr, const char *file, int line) {
    if (berry_svc) {
        berry_svc->puts("assert failed: ", 15);
        if (expr) berry_svc->puts(expr, (int)strlen(expr));
        berry_svc->puts(" at ", 4);
        if (file) berry_svc->puts(file, (int)strlen(file));
        berry_svc->putc('\n');
    }
    abort();
}

// --- time ------------------------------------------------------------------
// Seconds since 2020-01-01, derived from the boot clock plus a fixed base, so a
// build timestamp is monotonic and plausible (the loader has no wall clock).
time_t time(time_t *t) {
    time_t v = berry_svc->time_cs ? (time_t)(berry_svc->time_cs() / 100u) : 0;
    if (t) *t = v;
    return v;
}
int gettimeofday(void *tv, void *tz)  { (void)tz;
    if (tv) { long *p = (long *)tv; p[0] = (long)time(0); p[1] = 0; }   // {tv_sec, tv_usec}
    return 0;
}
// gmtime/localtime/mktime/difftime/strftime/asctime/ctime live in time.c.

// --- glob (stub: report "no matches", which the callers treat as no wildcard) ---
int glob(const char *pattern, int flags, void *errfn, void *pglob) {
    (void)pattern; (void)flags; (void)errfn; (void)pglob;
    return 3;                                    // GLOB_NOMATCH
}
void globfree(void *pglob) { (void)pglob; }

// ===========================================================================
// The POSIX low-level file layer, over the POD file services. fd 0/1/2 are the
// console streams; fd >= 3 index a small table of open card files.
// ===========================================================================
#define POD_FD_BASE 3
#define POD_FD_MAX  16
static int fd_handle[POD_FD_MAX];                // POD file handle for each fd, or 0

int open(const char *path, int flags, ...) {
    int mode;
    if (flags & O_WRONLY) mode = POD_OPEN_WRITE;                 // w
    else if (flags & O_RDWR) mode = (flags & O_CREAT) ? POD_OPEN_WRITE : POD_OPEN_UPDATE;
    else mode = POD_OPEN_READ;                                   // r
    if ((flags & O_CREAT) && (flags & O_TRUNC)) mode = POD_OPEN_WRITE;
    int h = berry_svc->file_open(path, mode);
    if (!h) return -1;
    for (int i = 0; i < POD_FD_MAX; i++) if (!fd_handle[i]) { fd_handle[i] = h; return POD_FD_BASE + i; }
    berry_svc->file_close(h);
    return -1;
}
int creat(const char *path, int mode) { (void)mode; return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0); }

static int fd_to_handle(int fd) {
    int i = fd - POD_FD_BASE;
    if (i < 0 || i >= POD_FD_MAX) return 0;
    return fd_handle[i];
}

// Detach an fd's POD handle and free its slot, transferring ownership to a FILE
// (this is how fdopen adopts a descriptor from open(); see stdio.c).
int __pod_fd_take(int fd) {
    int i = fd - POD_FD_BASE;
    if (i < 0 || i >= POD_FD_MAX) return 0;
    int h = fd_handle[i]; fd_handle[i] = 0;
    return h;
}

long read(int fd, void *buf, unsigned long count) {
    if (fd == 0) { unsigned char *p = buf; for (unsigned long i = 0; i < count; i++) p[i] = (unsigned char)(berry_svc->getkey() & 0xFF); return (long)count; }
    int h = fd_to_handle(fd);
    if (!h) return -1;
    return berry_svc->file_read(h, buf, (int)count);
}
long write(int fd, const void *buf, unsigned long count) {
    if (fd == 1 || fd == 2) { berry_svc->puts((const char *)buf, (int)count); return (long)count; }
    int h = fd_to_handle(fd);
    if (!h) return -1;
    return berry_svc->file_write(h, buf, (int)count);
}
long lseek(int fd, long off, int whence) {
    int h = fd_to_handle(fd);
    if (!h) return -1;
    return berry_svc->file_seek(h, off, whence);
}
int close(int fd) {
    int i = fd - POD_FD_BASE;
    if (i < 0 || i >= POD_FD_MAX || !fd_handle[i]) return (fd >= 0 && fd <= 2) ? 0 : -1;
    int r = berry_svc->file_close(fd_handle[i]);
    fd_handle[i] = 0;
    return r < 0 ? -1 : 0;
}
int unlink(const char *path) { return berry_svc->file_remove(path) < 0 ? -1 : 0; }
int __pod_remove(const char *path) { return unlink(path); }

char *getcwd(char *buf, unsigned long size) {   /* routes to the CAP_DIRS service */
    if (!buf || size == 0) return 0;
    if (berry_svc && berry_svc->getcwd && berry_svc->getcwd(buf, (int)size) >= 0) return buf;
    buf[0] = '.'; buf[1] = 0; return buf;       /* fallback: current directory */
}
int   chdir(const char *path) {                 /* routes to the CAP_DIRS service */
    return (berry_svc && berry_svc->chdir) ? berry_svc->chdir(path) : -1;
}

// mmap/mprotect exist only so tccrun.c (the -run JIT, unused in a POD) links;
// they always fail, so an attempted -run reports an error instead of running.
void *mmap(void *a, unsigned long l, int p, int f, int fd, long o) { (void)a; (void)l; (void)p; (void)f; (void)fd; (void)o; return (void *)-1; }
int   munmap(void *a, unsigned long l) { (void)a; (void)l; return -1; }
int   mprotect(void *a, unsigned long l, int p) { (void)a; (void)l; (void)p; return -1; }

// --- strtod / strtof (enough for C float constants: [+-]digits[.digits][eE[+-]digits]) ---
static int isws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
double strtod(const char *s, char **endptr) {
    const char *p = s;
    while (isws((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = (*p++ == '-');
    double val = 0.0; int any = 0;
    while (*p >= '0' && *p <= '9') { val = val * 10.0 + (*p++ - '0'); any = 1; }
    if (*p == '.') {
        p++;
        double f = 0.1;
        while (*p >= '0' && *p <= '9') { val += (*p++ - '0') * f; f *= 0.1; any = 1; }
    }
    if (any && (*p == 'e' || *p == 'E')) {
        const char *e = p + 1; int eneg = 0;
        if (*e == '+' || *e == '-') eneg = (*e++ == '-');
        if (*e >= '0' && *e <= '9') {
            int exp = 0;
            while (*e >= '0' && *e <= '9') exp = exp * 10 + (*e++ - '0');
            double scale = 1.0; while (exp--) scale *= 10.0;
            if (eneg) val /= scale; else val *= scale;
            p = e;
        }
    }
    if (endptr) *endptr = (char *)(any ? p : s);
    return neg ? -val : val;
}
float strtof(const char *s, char **endptr) { return (float)strtod(s, endptr); }
// long double conversion is fine now: the quad helpers come from lib-arm64.c.
long double strtold(const char *s, char **endptr) { return (long double)strtod(s, endptr); }
double atof(const char *s) { return strtod(s, 0); }
