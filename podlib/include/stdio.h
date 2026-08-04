#ifndef _POD_STDIO_H
#define _POD_STDIO_H
#include <stddef.h>
#include <stdarg.h>

typedef struct _POD_FILE FILE;

#define EOF (-1)
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
#define BUFSIZ 4096
#define FILENAME_MAX 260
#define L_tmpnam 260

FILE *__pod_stream(int which);
#define stdin  (__pod_stream(0))
#define stdout (__pod_stream(1))
#define stderr (__pod_stream(2))

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
int   fclose(FILE *fp);
int   fflush(FILE *fp);
int   fgetc(FILE *fp);
int   getc(FILE *fp);
int   getchar(void);
int   ungetc(int c, FILE *fp);
char *fgets(char *s, int size, FILE *fp);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
int   fputc(int c, FILE *fp);
int   putc(int c, FILE *fp);
int   putchar(int c);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
int   fputs(const char *s, FILE *fp);
int   puts(const char *s);
int   fseek(FILE *fp, long off, int whence);
long  ftell(FILE *fp);
void  rewind(FILE *fp);
int   feof(FILE *fp);
int   ferror(FILE *fp);
void  clearerr(FILE *fp);
int   remove(const char *path);
int   rename(const char *from, const char *to);

int printf(const char *fmt, ...);
int fprintf(FILE *fp, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *fp, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
void perror(const char *s);

#endif
