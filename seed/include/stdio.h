#ifndef SEED_STDIO_H
#define SEED_STDIO_H
// Freestanding <stdio.h> for native seeds. Backed by the seed file services, it
// gives a seed real read/write access to files on the SD card (long file names
// included), plus the familiar console streams, mirroring C stdio as closely as
// a bare-metal machine sensibly can.
//
// Provided: fopen/fclose/fflush, fread/fwrite, fgetc/getc/getchar, fputc/putc/
// putchar, fgets, fputs/puts, ungetc, fseek/ftell/rewind, feof/ferror/clearerr,
// remove, and the printf family (fprintf/printf/snprintf/vsnprintf/vfprintf).
// stdin/stdout/stderr are wired to the keyboard and screen.
//
// printf supports %d/i/u/o/x/X/c/s/p/% and %f/%e/%g; the float conversions use
// BASIC's number style (via the fmt_num service), so precision and the exact
// f/e/g choice are advisory. Not provided (no operating system underneath):
// tmpfile, freopen, popen, wide characters, and scanf - parse with fgets +
// strtol/strtod from <stdlib.h>.
#include <stddef.h>
#include <stdarg.h>

#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef EOF
#define EOF (-1)
#endif

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ   512

typedef struct _SEED_FILE FILE;

// stdin/stdout/stderr are accessor macros, not global pointer variables: a seed
// is a position-independent blob with no load-time relocations, so a pointer
// initialised to the address of a static object is not allowed. The accessor
// computes the address at run time (PC-relative), which is.
FILE *__seed_stream(int which);
#define stdin  __seed_stream(0)
#define stdout __seed_stream(1)
#define stderr __seed_stream(2)

FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *fp);
int    fflush(FILE *fp);

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);

int    fgetc(FILE *fp);
int    getc(FILE *fp);
int    getchar(void);
int    ungetc(int c, FILE *fp);
char  *fgets(char *s, int size, FILE *fp);

int    fputc(int c, FILE *fp);
int    putc(int c, FILE *fp);
int    putchar(int c);
int    fputs(const char *s, FILE *fp);
int    puts(const char *s);

int    fseek(FILE *fp, long off, int whence);
long   ftell(FILE *fp);
void   rewind(FILE *fp);

int    feof(FILE *fp);
int    ferror(FILE *fp);
void   clearerr(FILE *fp);

int    remove(const char *path);

int    printf(const char *fmt, ...)                 __attribute__((format(printf, 1, 2)));
int    fprintf(FILE *fp, const char *fmt, ...)      __attribute__((format(printf, 2, 3)));
int    snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int    vprintf(const char *fmt, va_list ap);
int    vfprintf(FILE *fp, const char *fmt, va_list ap);
int    vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

#endif
