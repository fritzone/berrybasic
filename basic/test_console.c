// In-memory console backend for the unit tests. It replaces host/console_host.c
// so a test can feed a canned script (tc_feed) and read back everything the
// interpreter printed (tc_output) without touching the real terminal.
#include <stdlib.h>
#include <string.h>
#include "console.h"

// ---- scripted input --------------------------------------------------------
static const char *in_buf;
static size_t      in_pos, in_len;

void tc_feed(const char *s) { in_buf = s; in_len = s ? strlen(s) : 0; in_pos = 0; }

// ---- captured output -------------------------------------------------------
static char  *out_buf;
static size_t out_len, out_cap;

void tc_reset_output(void) { out_len = 0; if (out_buf) out_buf[0] = 0; }
const char *tc_output(void) { return out_buf ? out_buf : ""; }

static void out_putc(char c) {
    if (out_len + 2 > out_cap) {
        out_cap = out_cap ? out_cap * 2 : 4096;
        out_buf = (char *)realloc(out_buf, out_cap);
    }
    out_buf[out_len++] = c;
    out_buf[out_len]   = 0;
}

void con_putc(char c)        { out_putc(c); }
void con_puts(const char *s) { while (*s) out_putc(*s++); }

int con_getline(char *buf, int maxlen) {
    if (in_pos >= in_len) return -1;                 // end of script
    int n = 0;
    while (in_pos < in_len && in_buf[in_pos] != '\n') {
        char c = in_buf[in_pos++];
        if (c != '\r' && n < maxlen - 1) buf[n++] = c;
    }
    if (in_pos < in_len && in_buf[in_pos] == '\n') in_pos++;
    buf[n] = 0;
    return n;
}

// Same contract as the host editor, but silent: no prompt/echo (so the captured
// output is exactly what the program printed). Keeps any prefill already in buf.
int con_getline_ed(char *buf, int maxlen, int prefill_len, const char *prompt) {
    (void)prompt;
    char tmp[1024];
    int t = con_getline(tmp, sizeof tmp);
    if (t < 0) return -1;
    int n = prefill_len;
    for (int i = 0; i < t && n < maxlen - 1; i++) buf[n++] = tmp[i];
    buf[n] = 0;
    return n;
}

// VDU: keep the same parameter-byte state machine as the real backends so
// multi-byte codes stay in sync, and mirror printable text to the capture.
void con_vdu(int b) {
    static const unsigned char nparams[32] = {
        0,1,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
        0,1,2,5,0,0,1,9, 8,5,0,1,4,4,0,2 };
    static int need = 0, cmd = -1, got = 0;
    b &= 0xff;
    if (cmd >= 0) { if (++got >= need) cmd = -1; return; }
    if (b < 32) {
        if (b == 10 || b == 13) out_putc((char)b);
        if (nparams[b]) { cmd = b; need = nparams[b]; got = 0; }
        return;
    }
    if (b == 127) { con_puts("\b \b"); return; }
    out_putc((char)b);
}

// ---- stubs: no screen, keyboard or clock in the harness --------------------
void con_cls(void) {}
void con_colour(int c) { (void)c; }
int  con_getkey(void) { return -1; }
unsigned long long con_micros(void) { static unsigned long long t; t += 10000; return t; }
int  con_inkey(int cs) { (void)cs; return -1; }
int  con_pos(void) { return 0; }
int  con_vpos(void) { return 0; }
int  con_splash(const char *b) { (void)b; return 0; }

void con_mode(int n) { (void)n; }

// In-memory current resolution, so SCREEN / SCREENW / SCREENH are exercised by
// the unit tests. Mirrors the kernel clamp and "startup" restore.
static int host_scr_w = 1280, host_scr_h = 1024;
int con_screen(int w, int h) {
    if (w <= 0 || h <= 0) { w = 1280; h = 1024; }
    if (w < 64)   w = 64;
    if (h < 64)   h = 64;
    if (w > 1920) w = 1920;
    if (h > 1080) h = 1080;
    host_scr_w = w; host_scr_h = h;
    return 1;
}
int con_screen_w(void) { return host_scr_w; }
int con_screen_h(void) { return host_scr_h; }

void con_gcol(int a, int c) { (void)a; (void)c; }
void con_plot(int code, int x, int y) { (void)code; (void)x; (void)y; }
void con_clg(void) {}
int  con_point(int x, int y) { (void)x; (void)y; return -1; }
void con_gcol_rgb(int r, int g, int b) { (void)r; (void)g; (void)b; }
void con_palette(int l, int r, int g, int b) { (void)l; (void)r; (void)g; (void)b; }
void con_line(int x1, int y1, int x2, int y2) { (void)x1; (void)y1; (void)x2; (void)y2; }
void con_rectangle(int x, int y, int w, int h, int f) { (void)x; (void)y; (void)w; (void)h; (void)f; }
void con_circle(int x, int y, int r, int f) { (void)x; (void)y; (void)r; (void)f; }
void con_ellipse(int x, int y, int rx, int ry, int f) { (void)x; (void)y; (void)rx; (void)ry; (void)f; }
void con_fill(int x, int y) { (void)x; (void)y; }
void con_sprite_get(long a, int x1, int y1, int x2, int y2) { (void)a; (void)x1; (void)y1; (void)x2; (void)y2; }
void con_sprite_put(long a, int x, int y) { (void)a; (void)x; (void)y; }
void con_mouse(int *x, int *y, int *b) { if (x) *x = 0; if (y) *y = 0; if (b) *b = 0; }
