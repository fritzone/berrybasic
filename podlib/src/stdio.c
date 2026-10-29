// <stdio.h> for the pod-libc, on the POD file + console services. A FILE is
// either a real card file (a storage handle) or one of the three console
// streams. Writes are unbuffered (the storage layer caches a sector), so fflush
// is a no-op and the card is always consistent.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "pod_rt.h"

enum { F_FILE = 0, F_COUT = 1, F_CIN = 2 };

struct _POD_FILE {
    int kind, fh, ungot, eof, err;
};

// All-integer initialisers, so no load-time relocation.
static FILE std_streams[3] = {
    { F_CIN,  0, -1, 0, 0 },   // stdin
    { F_COUT, 0, -1, 0, 0 },   // stdout
    { F_COUT, 0, -1, 0, 0 },   // stderr
};
FILE *__pod_stream(int which) {
    if (which < 0 || which > 2) which = 1;
    return &std_streams[which];
}

// --- open / close ----------------------------------------------------------
FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode) return NULL;
    int plus = 0;
    for (const char *p = mode; *p; p++) if (*p == '+') plus = 1;
    char m0 = mode[0];
    int smode, append = 0;
    if      (m0 == 'r') smode = plus ? POD_OPEN_UPDATE : POD_OPEN_READ;
    else if (m0 == 'w') smode = POD_OPEN_WRITE;
    else if (m0 == 'a') { smode = POD_OPEN_UPDATE; append = 1; }
    else return NULL;
    int fh = berry_svc->file_open(path, smode);
    if (!fh && append) fh = berry_svc->file_open(path, POD_OPEN_WRITE);
    if (!fh) return NULL;
    FILE *fp = malloc(sizeof(FILE));
    if (!fp) { berry_svc->file_close(fh); return NULL; }
    fp->kind = F_FILE; fp->fh = fh; fp->ungot = -1; fp->eof = 0; fp->err = 0;
    if (append) berry_svc->file_seek(fh, 0, SEEK_END);
    return fp;
}
extern int __pod_fd_take(int fd);
FILE *fdopen(int fd, const char *mode) {
    (void)mode;
    if (fd == 0) return stdin;
    if (fd == 1) return stdout;
    if (fd == 2) return stderr;
    int h = __pod_fd_take(fd);                   // adopt the descriptor's handle
    if (!h) return NULL;
    FILE *fp = malloc(sizeof(FILE));
    if (!fp) { berry_svc->file_close(h); return NULL; }
    fp->kind = F_FILE; fp->fh = h; fp->ungot = -1; fp->eof = 0; fp->err = 0;
    return fp;
}

int fclose(FILE *fp) {
    if (!fp || fp->kind != F_FILE) return 0;
    int r = berry_svc->file_close(fp->fh);
    free(fp);
    return r < 0 ? EOF : 0;
}
int fflush(FILE *fp) { (void)fp; return 0; }

// --- reading ---------------------------------------------------------------
int fgetc(FILE *fp) {
    if (!fp) return EOF;
    if (fp->ungot >= 0) { int c = fp->ungot; fp->ungot = -1; return c; }
    if (fp->kind == F_CIN) return berry_svc->getkey() & 0xFF;
    if (fp->kind != F_FILE) return EOF;
    unsigned char b;
    int n = berry_svc->file_read(fp->fh, &b, 1);
    if (n < 0) { fp->err = 1; return EOF; }
    if (n == 0) { fp->eof = 1; return EOF; }
    return b;
}
int getc(FILE *fp) { return fgetc(fp); }
int getchar(void)  { return fgetc(stdin); }
int ungetc(int c, FILE *fp) {
    if (!fp || c == EOF || fp->ungot >= 0) return EOF;
    fp->ungot = c & 0xFF; fp->eof = 0; return c & 0xFF;
}
char *fgets(char *s, int size, FILE *fp) {
    if (!s || size <= 0) return NULL;
    int i = 0;
    while (i < size - 1) { int c = fgetc(fp); if (c == EOF) break; s[i++] = (char)c; if (c == '\n') break; }
    if (i == 0) return NULL;
    s[i] = 0; return s;
}
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (!fp || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb, done = 0;
    unsigned char *p = ptr;
    if (fp->ungot >= 0) { p[done++] = (unsigned char)fp->ungot; fp->ungot = -1; }
    if (fp->kind == F_CIN) { while (done < total) p[done++] = (unsigned char)(berry_svc->getkey() & 0xFF); return done / size; }
    if (fp->kind != F_FILE) return 0;
    while (done < total) {
        int n = berry_svc->file_read(fp->fh, p + done, (int)(total - done));
        if (n < 0) { fp->err = 1; break; }
        if (n == 0) { fp->eof = 1; break; }
        done += (size_t)n;
    }
    return done / size;
}

// --- writing ---------------------------------------------------------------
int fputc(int c, FILE *fp) {
    if (!fp) return EOF;
    unsigned char b = (unsigned char)c;
    if (fp->kind == F_COUT) { berry_svc->putc(b); return b; }
    if (fp->kind != F_FILE) return EOF;
    int n = berry_svc->file_write(fp->fh, &b, 1);
    if (n < 1) { fp->err = 1; return EOF; }
    return b;
}
int putc(int c, FILE *fp) { return fputc(c, fp); }
int putchar(int c)        { return fputc(c, stdout); }
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (!fp || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    const unsigned char *p = ptr;
    if (fp->kind == F_COUT) { berry_svc->puts((const char *)p, (int)total); return nmemb; }
    if (fp->kind != F_FILE) return 0;
    size_t done = 0;
    while (done < total) {
        int n = berry_svc->file_write(fp->fh, p + done, (int)(total - done));
        if (n <= 0) { fp->err = 1; break; }
        done += (size_t)n;
    }
    return done / size;
}
int fputs(const char *s, FILE *fp) { size_t n = strlen(s); return fwrite(s, 1, n, fp) == n ? 0 : EOF; }
int puts(const char *s) { if (fputs(s, stdout) == EOF) return EOF; return fputc('\n', stdout) == EOF ? EOF : 0; }

// --- positioning -----------------------------------------------------------
int fseek(FILE *fp, long off, int whence) {
    if (!fp || fp->kind != F_FILE) return -1;
    fp->ungot = -1; fp->eof = 0;
    long r = berry_svc->file_seek(fp->fh, off, whence);
    return r < 0 ? -1 : 0;
}
long ftell(FILE *fp) {
    if (!fp || fp->kind != F_FILE) return -1;
    long r = berry_svc->file_seek(fp->fh, 0, SEEK_CUR);
    if (r < 0) return -1;
    if (fp->ungot >= 0 && r > 0) r--;
    return r;
}
void rewind(FILE *fp) { if (fp) { fseek(fp, 0, SEEK_SET); fp->err = 0; } }
int  feof(FILE *fp)     { return fp ? fp->eof : 1; }
int  ferror(FILE *fp)   { return fp ? fp->err : 1; }
void clearerr(FILE *fp) { if (fp) { fp->eof = 0; fp->err = 0; } }
int  remove(const char *path) { return berry_svc->file_remove(path) < 0 ? -1 : 0; }
int  rename(const char *a, const char *b) { (void)a; (void)b; return -1; }

// ===========================================================================
// printf family. A compact formatter over an sbuf sink: flags -,0,+,space,#,
// field width, '.'precision (incl. '*'), length modifiers l/ll/z/h, and the
// conversions d i u o x X p c s % and the float set f F e E g G.
// ===========================================================================
typedef struct { char *buf; size_t cap, len; } sbuf;
static void sb_putc(sbuf *s, char c) { if (s->buf && s->len + 1 < s->cap) s->buf[s->len] = c; s->len++; }
static void sb_pad(sbuf *s, char c, int n) { while (n-- > 0) sb_putc(s, c); }
static int u_to_str(unsigned long long v, int base, int upper, char *tmp) {
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do { tmp[n++] = digs[v % base]; v /= base; } while (v);
    return n;
}

// Minimal double formatter for the diagnostic %f/%e/%g tinycc emits. Not a
// correctly-rounded dtoa, but accurate to the requested precision for ordinary
// magnitudes, which is all the compiler's timing/aux output needs.
static double pl_pow10(int e) { double r = 1.0; while (e-- > 0) r *= 10.0; return r; }

static int fmt_double(char *out, double v, int prec, char conv) {
    int n = 0;
    if (v != v) { out[0] = 'n'; out[1] = 'a'; out[2] = 'n'; return 3; }
    if (prec < 0) prec = 6;
    if (v < 0) { out[n++] = '-'; v = -v; }
    double inf = 1e308 * 10;
    if (v == inf) { out[n++] = 'i'; out[n++] = 'n'; out[n++] = 'f'; return n; }

    if (conv == 'e' || conv == 'E') {
        int exp = 0;
        if (v != 0) { while (v >= 10.0) { v /= 10.0; exp++; } while (v < 1.0) { v *= 10.0; exp--; } }
        double scale = pl_pow10(prec);
        unsigned long long m = (unsigned long long)(v * scale + 0.5);
        char tmp[32]; int tn = u_to_str(m, 10, 0, tmp);
        while (tn < prec + 1) tmp[tn++] = '0';
        out[n++] = tmp[tn - 1];
        if (prec > 0) { out[n++] = '.'; for (int i = 0; i < prec; i++) out[n++] = tmp[tn - 2 - i]; }
        out[n++] = (conv == 'E') ? 'E' : 'e';
        out[n++] = exp < 0 ? '-' : '+';
        if (exp < 0) exp = -exp;
        out[n++] = (char)('0' + (exp / 10) % 10);
        out[n++] = (char)('0' + exp % 10);
        return n;
    }

    // %f / %g (g here just falls back to fixed): integer part + prec fraction.
    double scale = pl_pow10(prec);
    double rounded = v * scale + 0.5;
    unsigned long long scaled = (unsigned long long)rounded;
    unsigned long long ipart = scaled / (unsigned long long)scale;
    unsigned long long fpart = scaled % (unsigned long long)scale;
    char tmp[32]; int tn = u_to_str(ipart, 10, 0, tmp);
    while (tn > 0) out[n++] = tmp[--tn];
    if (prec > 0) {
        out[n++] = '.';
        char ftmp[32]; int fn = u_to_str(fpart, 10, 0, ftmp);
        for (int i = fn; i < prec; i++) out[n++] = '0';   // leading fraction zeros
        while (fn > 0) out[n++] = ftmp[--fn];
    }
    return n;
}

static int core_format(sbuf *s, const char *fmt, va_list ap) {
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { sb_putc(s, *f); continue; }
        f++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; f++) {
            if      (*f == '-') left = 1;
            else if (*f == '0') zero = 1;
            else if (*f == '+') plus = 1;
            else if (*f == ' ') space = 1;
            else if (*f == '#') alt = 1;
            else break;
        }
        int width = 0;
        if (*f == '*') { width = va_arg(ap, int); f++; if (width < 0) { left = 1; width = -width; } }
        else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        int prec = -1;
        if (*f == '.') { f++; prec = 0; if (*f == '*') { prec = va_arg(ap, int); f++; }
                         else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0'); }
        int lng = 0;                                     // 0=int 1=long 2=ll
        while (*f == 'l' || *f == 'z' || *f == 'h' || *f == 't' || *f == 'j') {
            if (*f == 'l') lng++; else if (*f == 'z' || *f == 't' || *f == 'j') lng = 2;
            f++;
        }

        char c = *f;
        char tmp[32]; int tn = 0; const char *pfx = ""; char sign = 0;
        if (c == 'd' || c == 'i') {
            long long v = (lng >= 2) ? va_arg(ap, long long) : (lng == 1) ? (long long)va_arg(ap, long) : (long long)va_arg(ap, int);
            int neg = v < 0;
            unsigned long long u = neg ? (unsigned long long)(-v) : (unsigned long long)v;
            sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
            tn = u_to_str(u, 10, 0, tmp);
        } else if (c == 'u' || c == 'x' || c == 'X' || c == 'o' || c == 'p') {
            unsigned long long u;
            if (c == 'p') { u = (unsigned long long)(uintptr_t)va_arg(ap, void *); pfx = "0x"; }
            else u = (lng >= 2) ? va_arg(ap, unsigned long long) : (lng == 1) ? (unsigned long long)va_arg(ap, unsigned long) : (unsigned long long)va_arg(ap, unsigned int);
            int base = (c == 'o') ? 8 : (c == 'u') ? 10 : 16;
            if (alt && (c == 'x' || c == 'X') && u) pfx = (c == 'X') ? "0X" : "0x";
            tn = u_to_str(u, base, c == 'X', tmp);
        } else if (c == 'f' || c == 'F' || c == 'e' || c == 'E' || c == 'g' || c == 'G') {
            char nb[64];
            int nn = fmt_double(nb, va_arg(ap, double), prec, c);
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
            int slen = 0; while (str[slen] && (prec < 0 || slen < prec)) slen++;
            int pad = width - slen;
            if (!left) sb_pad(s, ' ', pad);
            for (int i = 0; i < slen; i++) sb_putc(s, str[i]);
            if (left) sb_pad(s, ' ', pad);
            continue;
        } else if (c == '%') { sb_putc(s, '%'); continue; }
        else { sb_putc(s, '%'); if (c) sb_putc(s, c); continue; }

        // Precision on an integer conversion is the MINIMUM digit count (zero-
        // padded), e.g. "%.3d" of 33 -> "033". With an explicit precision the
        // '0' flag is ignored (C99).
        int is_int = (c == 'd' || c == 'i' || c == 'u' ||
                      c == 'x' || c == 'X' || c == 'o' || c == 'p');
        int zprec = (is_int && prec >= 0 && tn < prec) ? prec - tn : 0;
        int plen = (pfx[0] ? 2 : 0);
        int total = tn + zprec + (sign ? 1 : 0) + plen;
        int pad = width - total;
        int use_zero = zero && !left && !(is_int && prec >= 0);
        if (!left && !use_zero) sb_pad(s, ' ', pad);
        if (sign) sb_putc(s, sign);
        for (int i = 0; pfx[i]; i++) sb_putc(s, pfx[i]);
        if (use_zero) sb_pad(s, '0', pad);
        sb_pad(s, '0', zprec);                           // precision zeros
        while (tn > 0) sb_putc(s, tmp[--tn]);
        if (left) sb_pad(s, ' ', pad);
    }
    if (s->buf && s->cap) s->buf[s->len < s->cap ? s->len : s->cap - 1] = 0;
    return (int)s->len;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) { sbuf s = { buf, n, 0 }; return core_format(&s, fmt, ap); }
int snprintf(char *buf, size_t n, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, n, fmt, ap); va_end(ap); return r; }
int vsprintf(char *buf, const char *fmt, va_list ap) { sbuf s = { buf, (size_t)0x7fffffff, 0 }; return core_format(&s, fmt, ap); }
int sprintf(char *buf, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsprintf(buf, fmt, ap); va_end(ap); return r; }

int vfprintf(FILE *fp, const char *fmt, va_list ap) {
    va_list ap2; va_copy(ap2, ap);
    sbuf measure = { NULL, 0, 0 };
    int n = core_format(&measure, fmt, ap2);
    va_end(ap2);
    if (n < 0) return -1;
    // Format into a stack buffer for the common (short) case, so printf works
    // even in a POD that did not declare CAP_HEAP; only spill to malloc when big.
    char stackbuf[512];
    char *buf = ((size_t)n + 1 <= sizeof stackbuf) ? stackbuf : malloc((size_t)n + 1);
    if (!buf) return -1;
    sbuf build = { buf, (size_t)n + 1, 0 };
    core_format(&build, fmt, ap);
    int w = (int)fwrite(buf, 1, (size_t)n, fp);
    if (buf != stackbuf) free(buf);
    return w;
}
int fprintf(FILE *fp, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(fp, fmt, ap); va_end(ap); return r; }
int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }
int printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(stdout, fmt, ap); va_end(ap); return r; }

void perror(const char *s) { if (s && *s) { fputs(s, stderr); fputs(": ", stderr); } fputs("error\n", stderr); }

// --------------------------------------------------------------- scanf family
// A scanner over an abstract source (a string or a FILE) with one char of
// push-back. Reads decimal/hex/octal integers, floats, chars, %s runs and
// scansets, with optional width, '*' suppression and length modifiers.
typedef struct { const char *s; FILE *f; int pushed; int count; } scan_src;

static int sc_get(scan_src *sc) {
    sc->count++;
    if (sc->pushed != -2) { int c = sc->pushed; sc->pushed = -2; return c; }
    if (sc->f) return fgetc(sc->f);
    unsigned char c = (unsigned char)*sc->s;
    if (!c) return -1;
    sc->s++;
    return c;
}
static void sc_unget(scan_src *sc, int c) { sc->pushed = c; sc->count--; }
static int sc_isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

static int core_scan(scan_src *sc, const char *fmt, va_list ap) {
    int assigned = 0;
    for (; *fmt; fmt++) {
        if (sc_isspace((unsigned char)*fmt)) {
            int c; do { c = sc_get(sc); } while (sc_isspace(c));
            if (c != -1) sc_unget(sc, c);
            continue;
        }
        if (*fmt != '%') {
            int c = sc_get(sc);
            if (c != (unsigned char)*fmt) { if (c != -1) sc_unget(sc, c); return assigned; }
            continue;
        }
        fmt++;                                            // past '%'
        if (*fmt == '%') { int c = sc_get(sc); if (c != '%') { if (c!=-1) sc_unget(sc,c); return assigned; } continue; }
        int suppress = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }
        int width = 0; while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int lng = 0;                                      // 0=int/float, 1=long/double, 2=long long
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'L') { if (*fmt=='l'||*fmt=='L') lng++; fmt++; }
        char conv = *fmt;
        if (!conv) break;

        if (conv != 'c' && conv != '[' && conv != 'n') {  // skip leading whitespace
            int c; do { c = sc_get(sc); } while (sc_isspace(c));
            if (c == -1) return assigned; sc_unget(sc, c);
        }
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o' || conv == 'p') {
            int base = (conv=='x'||conv=='X'||conv=='p') ? 16 : (conv=='o') ? 8 : 10;
            unsigned long long acc = 0; int neg = 0, any = 0, n = 0;
            int c = sc_get(sc);
            if (c == '+' || c == '-') { neg = (c=='-'); c = sc_get(sc); n++; }
            if ((conv=='i' || base==16) && c=='0') { int d=sc_get(sc); if (d=='x'||d=='X'){base=16;c=sc_get(sc);n+=2;} else {any=1; if(conv=='i')base=8; sc_unget(sc,d);} }
            while (c != -1 && (!width || n < width)) {
                int dv;
                if (c>='0'&&c<='9') dv=c-'0'; else if (c>='a'&&c<='f') dv=c-'a'+10; else if (c>='A'&&c<='F') dv=c-'A'+10; else break;
                if (dv >= base) break;
                acc = acc * base + dv; any = 1; n++; c = sc_get(sc);
            }
            if (c != -1) sc_unget(sc, c);
            if (!any) return assigned;
            long long v = neg ? -(long long)acc : (long long)acc;
            if (!suppress) {
                if (conv=='u'||conv=='x'||conv=='X'||conv=='o'||conv=='p') {
                    if (lng>=2) *va_arg(ap, unsigned long long*) = acc;
                    else if (lng==1) *va_arg(ap, unsigned long*) = (unsigned long)acc;
                    else *va_arg(ap, unsigned*) = (unsigned)acc;
                } else {
                    if (lng>=2) *va_arg(ap, long long*) = v;
                    else if (lng==1) *va_arg(ap, long*) = (long)v;
                    else *va_arg(ap, int*) = (int)v;
                }
                assigned++;
            }
        } else if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G' || conv == 'a') {
            char nbuf[64]; int n = 0;
            int c = sc_get(sc);
            if (c=='+'||c=='-') { if(n<63)nbuf[n++]=(char)c; c=sc_get(sc); }
            while (c>='0'&&c<='9' && (!width||n<width)) { if(n<63)nbuf[n++]=(char)c; c=sc_get(sc); }
            if (c=='.') { if(n<63)nbuf[n++]='.'; c=sc_get(sc); while (c>='0'&&c<='9'&&(!width||n<width)){ if(n<63)nbuf[n++]=(char)c; c=sc_get(sc);} }
            if (c=='e'||c=='E') { if(n<63)nbuf[n++]=(char)c; c=sc_get(sc); if(c=='+'||c=='-'){if(n<63)nbuf[n++]=(char)c;c=sc_get(sc);} while(c>='0'&&c<='9'){if(n<63)nbuf[n++]=(char)c;c=sc_get(sc);} }
            if (c != -1) sc_unget(sc, c);
            if (n == 0) return assigned;
            nbuf[n] = 0;
            double d = strtod(nbuf, 0);
            if (!suppress) { if (lng>=1) *va_arg(ap, double*) = d; else *va_arg(ap, float*) = (float)d; assigned++; }
        } else if (conv == 's') {
            char *out = suppress ? 0 : va_arg(ap, char*); int n = 0;
            int c = sc_get(sc);
            while (c != -1 && !sc_isspace(c) && (!width || n < width)) { if (out) out[n] = (char)c; n++; c = sc_get(sc); }
            if (c != -1) sc_unget(sc, c);
            if (n == 0) return assigned;
            if (out) out[n] = 0;
            if (!suppress) assigned++;
        } else if (conv == 'c') {
            int w = width ? width : 1;
            char *out = suppress ? 0 : va_arg(ap, char*); int n = 0;
            for (; n < w; n++) { int c = sc_get(sc); if (c == -1) break; if (out) out[n] = (char)c; }
            if (n < w) return assigned;
            if (!suppress) assigned++;
        } else if (conv == '[') {
            fmt++;
            int negset = 0; if (*fmt == '^') { negset = 1; fmt++; }
            const char *set = fmt;
            if (*fmt == ']') fmt++;                        // a ']' right after is a literal
            while (*fmt && *fmt != ']') fmt++;
            int setlen = (int)(fmt - set);
            char *out = suppress ? 0 : va_arg(ap, char*); int n = 0;
            int c = sc_get(sc);
            while (c != -1 && (!width || n < width)) {
                int in = 0; for (int i = 0; i < setlen; i++) if ((unsigned char)set[i] == c) { in = 1; break; }
                if (in == negset) break;
                if (out) out[n] = (char)c; n++; c = sc_get(sc);
            }
            if (c != -1) sc_unget(sc, c);
            if (n == 0) return assigned;
            if (out) out[n] = 0;
            if (!suppress) assigned++;
        } else if (conv == 'n') {
            if (!suppress) { *va_arg(ap, int*) = sc->count; }
        } else {
            return assigned;
        }
    }
    return assigned;
}

int vsscanf(const char *str, const char *fmt, va_list ap) {
    scan_src sc = { str, 0, -2, 0 };
    return core_scan(&sc, fmt, ap);
}
int sscanf(const char *str, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vsscanf(str, fmt, ap); va_end(ap); return r;
}
int vfscanf(FILE *fp, const char *fmt, va_list ap) {
    scan_src sc = { 0, fp, -2, 0 };
    return core_scan(&sc, fmt, ap);
}
int fscanf(FILE *fp, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vfscanf(fp, fmt, ap); va_end(ap); return r;
}
long getdelim(char **lineptr, size_t *n, int delim, FILE *fp) {
    if (!lineptr || !n) return -1;
    if (!*lineptr || *n == 0) { *n = 128; *lineptr = malloc(*n); if (!*lineptr) return -1; }
    size_t i = 0; int c;
    while ((c = fgetc(fp)) != -1) {
        if (i + 1 >= *n) { size_t nn = *n * 2; char *p = realloc(*lineptr, nn); if (!p) return -1; *lineptr = p; *n = nn; }
        (*lineptr)[i++] = (char)c;
        if (c == delim) break;
    }
    if (i == 0) return -1;
    (*lineptr)[i] = 0;
    return (long)i;
}
long getline(char **lineptr, size_t *n, FILE *fp) { return getdelim(lineptr, n, '\n', fp); }

int vscanf(const char *fmt, va_list ap) { return vfscanf(stdin, fmt, ap); }
int scanf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vfscanf(stdin, fmt, ap); va_end(ap); return r;
}

void setbuf(FILE *fp, char *buf) { (void)fp; (void)buf; }
