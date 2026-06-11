// Host (Linux) console backend for the BASIC interpreter. Lets us build and
// unit-test the interpreter natively with gdb/valgrind before running it on
// the bare-metal target.
#include <stdio.h>
#include <string.h>
#include "console.h"

void con_putc(char c) { putchar(c); }

void con_puts(const char *s) { fputs(s, stdout); }

int con_getline(char *buf, int maxlen) {
    if (!fgets(buf, maxlen, stdin)) return -1;
    int n = (int)strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    return n;
}

void con_cls(void) { fputs("\033[2J\033[H", stdout); }   // ANSI clear on a terminal

void con_colour(int c) {
    static const int ansi[8] = { 30, 31, 32, 33, 34, 35, 36, 37 };
    printf("\033[%dm", ansi[c & 7]);
}

int con_getkey(void) {
    int ch = getchar();
    return ch < 0 ? 13 : ch;     // EOF -> treat as Return
}

#include <sys/time.h>
unsigned long long con_micros(void) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (unsigned long long)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

int con_inkey(int centiseconds) { (void)centiseconds; return -1; }   // no raw stdin on host
int con_pos(void)  { return 0; }
int con_vpos(void) { return 0; }
int con_splash(const char *banner) { (void)banner; return 0; }   // no logo on host; caller prints banner

// Graphics are framebuffer-only; the host backend has no display, so these are
// no-ops (POINT always reports "off-screen").
void con_mode(int n) { (void)n; }
void con_gcol(int action, int colour) { (void)action; (void)colour; }
void con_plot(int code, int x, int y) { (void)code; (void)x; (void)y; }
void con_clg(void) {}
int  con_point(int x, int y) { (void)x; (void)y; return -1; }
