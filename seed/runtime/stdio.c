// Seed runtime: a freestanding <stdio.h> implemented on the seed file services.
// A FILE is either a real SD-card file (a storage handle) or one of the three
// console streams. Writes are unbuffered (the storage layer already caches a
// sector), so fflush is a no-op and data on the card is always consistent.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "seed.h"

enum { F_FILE = 0, F_COUT = 1, F_CIN = 2 };   // stream kind

struct _SEED_FILE {
    int kind;        // F_FILE / F_COUT / F_CIN
    int fh;          // storage handle (F_FILE only)
    int ungot;       // pushed-back byte, or -1
    int eof, err;    // sticky end-of-file / error flags
};

// The three console streams live in one static array (all-integer initialisers,
// so no load-time relocation). __seed_stream computes each address at run time.
static FILE std_streams[3] = {
    { F_CIN,  0, -1, 0, 0 },   // 0: stdin
    { F_COUT, 0, -1, 0, 0 },   // 1: stdout
    { F_COUT, 0, -1, 0, 0 },   // 2: stderr
};
FILE *__seed_stream(int which) {
    if (which < 0 || which > 2) which = 1;
    return &std_streams[which];
}

// --- open / close -----------------------------------------------------------

FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode) return NULL;
    int plus = 0;
    for (const char *p = mode; *p; p++) if (*p == '+') plus = 1;
    char m0 = mode[0];
    int smode, append = 0;
    if      (m0 == 'r') smode = plus ? SEED_FOPEN_UPDATE : SEED_FOPEN_READ;
    else if (m0 == 'w') smode = SEED_FOPEN_WRITE;
    else if (m0 == 'a') { smode = SEED_FOPEN_UPDATE; append = 1; }
    else return NULL;

    int fh = seed_svc->file_open(path, smode);
    if (!fh && append) fh = seed_svc->file_open(path, SEED_FOPEN_WRITE);  // create for append
    if (!fh) return NULL;

    FILE *fp = malloc(sizeof(FILE));
    if (!fp) { seed_svc->file_close(fh); return NULL; }
    fp->kind = F_FILE; fp->fh = fh; fp->ungot = -1; fp->eof = 0; fp->err = 0;
    if (append) seed_svc->file_seek(fh, 0, SEEK_END);
    return fp;
}

int fclose(FILE *fp) {
    if (!fp || fp->kind != F_FILE) return 0;
    int r = seed_svc->file_close(fp->fh);
    free(fp);
    return r < 0 ? EOF : 0;
}

int fflush(FILE *fp) { (void)fp; return 0; }   // unbuffered

// --- reading ----------------------------------------------------------------

int fgetc(FILE *fp) {
    if (!fp) return EOF;
    if (fp->ungot >= 0) { int c = fp->ungot; fp->ungot = -1; return c; }
    if (fp->kind == F_CIN) return seed_svc->getkey() & 0xFF;
    if (fp->kind != F_FILE) return EOF;
    unsigned char b;
    int n = seed_svc->file_read(fp->fh, &b, 1);
    if (n < 0) { fp->err = 1; return EOF; }
    if (n == 0) { fp->eof = 1; return EOF; }
    return b;
}
int getc(FILE *fp)   { return fgetc(fp); }
int getchar(void)    { return fgetc(stdin); }

int ungetc(int c, FILE *fp) {
    if (!fp || c == EOF || fp->ungot >= 0) return EOF;
    fp->ungot = c & 0xFF;
    fp->eof = 0;
    return c & 0xFF;
}

char *fgets(char *s, int size, FILE *fp) {
    if (!s || size <= 0) return NULL;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(fp);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;                    // nothing before EOF
    s[i] = 0;
    return s;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (!fp || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb, done = 0;
    unsigned char *p = ptr;
    if (fp->ungot >= 0) { p[done++] = (unsigned char)fp->ungot; fp->ungot = -1; }
    if (fp->kind == F_CIN) {
        while (done < total) p[done++] = (unsigned char)(seed_svc->getkey() & 0xFF);
        return done / size;
    }
    if (fp->kind != F_FILE) return 0;
    while (done < total) {
        int n = seed_svc->file_read(fp->fh, p + done, (int)(total - done));
        if (n < 0) { fp->err = 1; break; }
        if (n == 0) { fp->eof = 1; break; }
        done += (size_t)n;
    }
    return done / size;
}

// --- writing ----------------------------------------------------------------

int fputc(int c, FILE *fp) {
    if (!fp) return EOF;
    unsigned char b = (unsigned char)c;
    if (fp->kind == F_COUT) { seed_svc->putc(b); return b; }
    if (fp->kind != F_FILE) return EOF;
    int n = seed_svc->file_write(fp->fh, &b, 1);
    if (n < 1) { fp->err = 1; return EOF; }
    return b;
}
int putc(int c, FILE *fp) { return fputc(c, fp); }
int putchar(int c)        { return fputc(c, stdout); }

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (!fp || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    const unsigned char *p = ptr;
    if (fp->kind == F_COUT) { seed_svc->puts((const char *)p, (int)total); return nmemb; }
    if (fp->kind != F_FILE) return 0;
    size_t done = 0;
    while (done < total) {
        int n = seed_svc->file_write(fp->fh, p + done, (int)(total - done));
        if (n <= 0) { fp->err = 1; break; }
        done += (size_t)n;
    }
    return done / size;
}

int fputs(const char *s, FILE *fp) {
    size_t n = 0; while (s[n]) n++;
    return fwrite(s, 1, n, fp) == n ? 0 : EOF;
}
int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

// --- positioning ------------------------------------------------------------

int fseek(FILE *fp, long off, int whence) {
    if (!fp || fp->kind != F_FILE) return -1;
    fp->ungot = -1; fp->eof = 0;
    long r = seed_svc->file_seek(fp->fh, off, whence);
    return r < 0 ? -1 : 0;
}
long ftell(FILE *fp) {
    if (!fp || fp->kind != F_FILE) return -1;
    long r = seed_svc->file_seek(fp->fh, 0, SEEK_CUR);
    if (r < 0) return -1;
    if (fp->ungot >= 0 && r > 0) r--;          // a pushed-back byte = one earlier
    return r;
}
void rewind(FILE *fp) { if (fp) { fseek(fp, 0, SEEK_SET); fp->err = 0; } }

int  feof(FILE *fp)      { return fp ? fp->eof : 1; }
int  ferror(FILE *fp)    { return fp ? fp->err : 1; }
void clearerr(FILE *fp)  { if (fp) { fp->eof = 0; fp->err = 0; } }

int remove(const char *path) { return seed_svc->file_remove(path) < 0 ? -1 : 0; }

// --- printf family ----------------------------------------------------------
// A compact integer/string formatter shared by every entry point. Supports the
// flags '-', '0', '+' and ' ', a field width, the length modifiers l/ll/z, and
// the conversions d i u o x X p c s %. '+' and ' ' apply to the signed
// conversions (d/i) only, as in standard C: '+' always shows a sign, ' ' puts a
// space where the '-' would go. Floating point (%f/%e/%g) is not supported.

typedef struct { char *buf; size_t cap, len; } sbuf;

static void sb_putc(sbuf *s, char c) {
    if (s->buf && s->len + 1 < s->cap) s->buf[s->len] = c;
    s->len++;
}
static void sb_pad(sbuf *s, char c, int n) { while (n-- > 0) sb_putc(s, c); }

// Format one unsigned value in `base` into tmp (reversed), return its length.
static int u_to_str(unsigned long long v, int base, int upper, char *tmp) {
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do { tmp[n++] = digs[v % base]; v /= base; } while (v);
    return n;
}

static int core_format(sbuf *s, const char *fmt, va_list ap) {
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { sb_putc(s, *f); continue; }
        f++;
        int left = 0, zero = 0, plus = 0, space = 0;
        for (;; f++) {
            if      (*f == '-') left  = 1;
            else if (*f == '0') zero  = 1;
            else if (*f == '+') plus  = 1;
            else if (*f == ' ') space = 1;
            else break;
        }
        int width = 0;
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        int lng = 0;                                    // 0=int 1=long 2=long long
        while (*f == 'l' || *f == 'z') { lng = (*f == 'z') ? 2 : lng + 1; f++; }

        char c = *f;
        char tmp[24]; int tn = 0; const char *pfx = ""; char sign = 0;
        if (c == 'd' || c == 'i') {
            long long v = (lng >= 2) ? va_arg(ap, long long)
                        : (lng == 1) ? (long long)va_arg(ap, long)
                                     : (long long)va_arg(ap, int);
            int neg = v < 0;
            unsigned long long u = neg ? (unsigned long long)(-v)
                                       : (unsigned long long)v;
            sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
            tn = u_to_str(u, 10, 0, tmp);
        } else if (c == 'u' || c == 'x' || c == 'X' || c == 'o' || c == 'p') {
            unsigned long long u;
            if (c == 'p') { u = (unsigned long long)(uintptr_t)va_arg(ap, void *); pfx = "0x"; }
            else u = (lng >= 2) ? va_arg(ap, unsigned long long)
                   : (lng == 1) ? (unsigned long long)va_arg(ap, unsigned long)
                                : (unsigned long long)va_arg(ap, unsigned int);
            int base = (c == 'o') ? 8 : (c == 'u') ? 10 : 16;
            tn = u_to_str(u, base, c == 'X', tmp);
        } else if (c == 'f' || c == 'F' || c == 'e' || c == 'E' ||
                   c == 'g' || c == 'G') {
            // Float conversions use BASIC's number style (via the fmt_num
            // service): the exact PRINT/STR$ output. The f/e/g choice and any
            // precision are advisory; width padding (space) is honoured.
            char nb[32];
            int nn = seed_svc->fmt_num(va_arg(ap, double), nb);
            int pad = width - nn;
            if (!left) sb_pad(s, ' ', pad);
            for (int i = 0; i < nn; i++) sb_putc(s, nb[i]);
            if (left) sb_pad(s, ' ', pad);
            continue;
        } else if (c == 'c') {
            tmp[tn++] = (char)va_arg(ap, int);
        } else if (c == 's') {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            int slen = 0; while (str[slen]) slen++;
            int pad = width - slen;
            if (!left) sb_pad(s, ' ', pad);
            for (int i = 0; i < slen; i++) sb_putc(s, str[i]);
            if (left) sb_pad(s, ' ', pad);
            continue;
        } else if (c == '%') {
            sb_putc(s, '%'); continue;
        } else {                                        // unknown: emit literally
            sb_putc(s, '%'); if (c) sb_putc(s, c); continue;
        }

        int plen = (pfx[0] ? 2 : 0);
        int total = tn + (sign ? 1 : 0) + plen;
        int pad = width - total;
        if (!left && !zero) sb_pad(s, ' ', pad);
        if (sign) sb_putc(s, sign);                     // '-', '+' or ' '
        for (int i = 0; pfx[i]; i++) sb_putc(s, pfx[i]);
        if (!left && zero)  sb_pad(s, '0', pad);
        while (tn > 0) sb_putc(s, tmp[--tn]);           // digits are reversed
        if (left) sb_pad(s, ' ', pad);
    }
    if (s->buf && s->cap) s->buf[s->len < s->cap ? s->len : s->cap - 1] = 0;
    return (int)s->len;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    sbuf s = { buf, n, 0 };
    return core_format(&s, fmt, ap);
}
int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int vfprintf(FILE *fp, const char *fmt, va_list ap) {
    va_list ap2; va_copy(ap2, ap);
    sbuf measure = { NULL, 0, 0 };                      // pass 1: how long?
    int n = core_format(&measure, fmt, ap2);
    va_end(ap2);
    if (n < 0) return -1;
    char *buf = malloc((size_t)n + 1);
    if (!buf) return -1;
    sbuf build = { buf, (size_t)n + 1, 0 };
    core_format(&build, fmt, ap);
    int w = (int)fwrite(buf, 1, (size_t)n, fp);
    free(buf);
    return w;
}
int fprintf(FILE *fp, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(fp, fmt, ap);
    va_end(ap);
    return r;
}
int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }
int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}
