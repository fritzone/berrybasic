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

// Host has no interactive line editor (stdin is line-buffered), so there's no
// cursor editing or history. We still honour the prompt and the prefill so AUTO
// and EDIT work when scripting: print the prompt + prefill, then append the
// typed line after the prefilled characters.
int con_getline_ed(char *buf, int maxlen, int prefill_len, const char *prompt) {
    if (prompt && *prompt) fputs(prompt, stdout);
    if (prefill_len > 0) { buf[prefill_len] = 0; fputs(buf, stdout); }
    fflush(stdout);
    char tmp[1024];
    if (!fgets(tmp, sizeof tmp, stdin)) return -1;
    int t = (int)strlen(tmp);
    while (t > 0 && (tmp[t - 1] == '\n' || tmp[t - 1] == '\r')) tmp[--t] = 0;
    int n = prefill_len;
    for (int i = 0; i < t && n < maxlen - 1; i++) buf[n++] = tmp[i];
    buf[n] = 0;
    return n;
}

void con_cls(void) { fputs("\033[2J\033[H", stdout); }   // ANSI clear on a terminal

// VDU driver for the host: keeps the same parameter-counting state machine as
// the target so multi-byte commands stay in sync, prints text codes to stdout,
// and ignores the graphics/screen-control codes (no display on the host).
void con_vdu(int b) {
    static const unsigned char nparams[32] = {
        0,1,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        0,1,2,5,0,0,1,9, 8,5,0,1,4,4,0,2
    };
    static int need = 0, cmd = -1, got = 0;
    b &= 0xff;
    if (cmd >= 0) { if (++got >= need) cmd = -1; return; }   // skip parameter bytes
    if (b < 32) {
        if (b == 10 || b == 13) putchar(b);                  // LF/CR are visible
        if (nparams[b]) { cmd = b; need = nparams[b]; got = 0; }
        return;
    }
    if (b == 127) { fputs("\b \b", stdout); return; }
    putchar(b);                                              // printable
}

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

// No real framebuffer on the host, but keep a plausible current resolution so
// SCREEN / SCREENW / SCREENH behave (and are testable). Mirrors the kernel clamp.
static int host_scr_w = 1280, host_scr_h = 1024;
int con_screen(int w, int h) {
    if (w <= 0 || h <= 0) { w = 1280; h = 1024; }        // restore "startup"
    if (w < 64)   w = 64;
    if (h < 64)   h = 64;
    if (w > 1920) w = 1920;
    if (h > 1080) h = 1080;
    host_scr_w = w; host_scr_h = h;
    return 1;
}
int con_screen_w(void) { return host_scr_w; }
int con_screen_h(void) { return host_scr_h; }

void con_gcol(int action, int colour) { (void)action; (void)colour; }
void con_plot(int code, int x, int y) { (void)code; (void)x; (void)y; }
void con_clg(void) {}
int  con_point(int x, int y) { (void)x; (void)y; return -1; }

// Graphics library: no framebuffer on the host, so these are no-ops.
void con_gcol_rgb(int r, int g, int b) { (void)r; (void)g; (void)b; }
void con_palette(int l, int r, int g, int b) { (void)l; (void)r; (void)g; (void)b; }
void con_line(int x1, int y1, int x2, int y2) { (void)x1; (void)y1; (void)x2; (void)y2; }
void con_rectangle(int x, int y, int w, int h, int f) { (void)x; (void)y; (void)w; (void)h; (void)f; }
void con_circle(int x, int y, int r, int f) { (void)x; (void)y; (void)r; (void)f; }
void con_ellipse(int x, int y, int rx, int ry, int f) { (void)x; (void)y; (void)rx; (void)ry; (void)f; }
void con_fill(int x, int y) { (void)x; (void)y; }
void con_sprite_get(long a, int x1, int y1, int x2, int y2) { (void)a; (void)x1; (void)y1; (void)x2; (void)y2; }
void con_sprite_put(long a, int x, int y) { (void)a; (void)x; (void)y; }

// No mouse on the host backend; report a parked pointer with no buttons.
void con_mouse(int *x, int *y, int *buttons) {
    if (x) *x = 0;
    if (y) *y = 0;
    if (buttons) *buttons = 0;
}
