#include <stdint.h>
#include "uart.h"
#include "mailbox.h"
#include "graphics.h"
#include "font.h"
#include "usb_kbd.h"
#include "console.h"
#include "basic.h"
#include "mmu.h"
#include "sd.h"
#include "storage.h"
#include "pcie.h"
#include "xhci.h"

// ---------------------------------------------------------------------------
// Display configuration
// Override at build time with e.g.  make CFLAGS_EXTRA="-DFB_WIDTH=1024 -DFB_HEIGHT=768"
// ---------------------------------------------------------------------------
#ifndef FB_WIDTH
#define FB_WIDTH  800
#endif
#ifndef FB_HEIGHT
#define FB_HEIGHT 600
#endif

// CHAR_W / CHAR_H come from font.h (BBC 8x8 glyph scaled by FONT_SCALE).
#define TERM_COLS (FB_WIDTH  / CHAR_W)
#define TERM_ROWS (FB_HEIGHT / CHAR_H)

// Free-running 1 MHz system timer (low 32 bits); used to pace the cursor blink.
#define TIMER_CLO       (*(volatile uint32_t *)(0xFE003004UL))
#define CURSOR_BLINK_US 500000u   // half-period: on 500 ms, off 500 ms

static int cursor_col     = 0;
static int cursor_row     = 0;
static int fb_ready       = 0;
static uint32_t g_fb_phys = 0;   // framebuffer physical base (for non-cacheable remap)
static uint32_t g_fb_size = 0;   // framebuffer size in bytes
static int cursor_visible = 0;   // whether the cursor glyph is currently drawn
static uint32_t text_color = COLOR_WHITE;   // current foreground (BBC COLOUR)

// BBC logical colours 0..7. This framebuffer is RGBA byte-order (low byte = R),
// so values are written as 0xAABBGGRR to get true colours.
static const uint32_t bbc_palette[8] = {
    0xFF000000,  // 0 black
    0xFF0000FF,  // 1 red
    0xFF00FF00,  // 2 green
    0xFF00FFFF,  // 3 yellow
    0xFFFF0000,  // 4 blue
    0xFFFF00FF,  // 5 magenta
    0xFFFFFF00,  // 6 cyan
    0xFFFFFFFF,  // 7 white
};

// ---------------------------------------------------------------------------
// Terminal
// ---------------------------------------------------------------------------

// BBC-style flashing block cursor at the current cell. show=1 draws it, show=0
// erases it. The cursor always sits on the (empty) next-write cell, so erasing
// to black never clobbers a real glyph.
static void cursor_set(int show) {
    if (!fb_ready) return;
    int px = cursor_col * CHAR_W;
    int py = cursor_row * CHAR_H;
    bar(px, py, px + CHAR_W - 1, py + CHAR_H - 1,
        show ? COLOR_WHITE : COLOR_BLACK);
    cursor_visible = show;
}

static void term_advance(void) {
    cursor_col = 0;
    if (++cursor_row >= TERM_ROWS) {
        scroll_up(CHAR_H, COLOR_BLACK);   // scroll one text row up
        cursor_row     = TERM_ROWS - 1;   // stay on the (new) bottom row
        cursor_visible = 0;               // the scroll wiped any drawn cursor
    }
}

void term_putchar(char c) {
    if (!fb_ready) return;
    if (cursor_visible) cursor_set(0);   // lift cursor before editing the cell
    if (c == '\r') { cursor_col = 0; return; }
    if (c == '\n') { term_advance(); return; }
    if (c == '\t') {
        cursor_col = (cursor_col + 4) & ~3;
        if (cursor_col >= TERM_COLS) term_advance();
        return;
    }
    if (c == '\b') {
        if (cursor_col > 0) --cursor_col;
        else if (cursor_row > 0) { --cursor_row; cursor_col = TERM_COLS - 1; }
        else return;
        bar(cursor_col * CHAR_W, cursor_row * CHAR_H,
            cursor_col * CHAR_W + CHAR_W - 1, cursor_row * CHAR_H + CHAR_H - 1,
            COLOR_BLACK);
        return;
    }
    if (c < 32 || c > 126) return;
    int px = cursor_col * CHAR_W;
    int py = cursor_row * CHAR_H;
    bar(px, py, px + CHAR_W - 1, py + CHAR_H - 1, COLOR_BLACK);
    draw_char(c, px, py, text_color);
    if (++cursor_col >= TERM_COLS) term_advance();
}

static void term_puts(const char *s) {
    while (*s) term_putchar(*s++);
}

// ---------------------------------------------------------------------------
// Line editor
// ---------------------------------------------------------------------------

static int g_kbd_ok  = 0;   // DWC2 (USB-C) keyboard present
static int g_xhci_ok = 0;   // xHCI/VL805 (USB-A) keyboard present
static int g_sd_ready = 0;  // SD filesystem mounted (boot log can be written)

// Dump the in-RAM UART log to BOOTLOG.TXT on the SD card. Called only from the
// exception handler now, so a normal boot leaves the data partition holding just
// the user's programs; a crash still surfaces diagnostics on a board with no
// serial cable. Best-effort: needs the filesystem mounted, and guards against
// re-entry (e.g. an exception while already writing).
static void flush_boot_log(void) {
    static int busy = 0;
    if (!g_sd_ready || busy) return;
    busy = 1;
    const char *buf; int len;
    uart_log_get(&buf, &len);
    stg_write("BOOTLOG.TXT", buf, len);
    busy = 0;
}

// Emit a boot-progress line to *both* the UART/RAM log and, once it exists, the
// HDMI framebuffer. On a cableless board the screen is the only channel we know
// works (the firmware drew its own diagnostics on it), so mirroring boot stages
// there means the last line visible on screen is exactly where the kernel hung.
static void boot_msg(const char *s) {
    uart_puts(s);
    if (fb_ready) term_puts(s);
}

// Return the next input character from any keyboard (DWC2 USB-C or xHCI USB-A),
// falling back to the UART, or 0 if nothing is pending.
static char term_pollchar(void) {
    char c = 0;
    if (g_kbd_ok)  c = usb_kbd_getchar();
    if (!c && g_xhci_ok) c = xhci_kbd_getchar();
    if (!c)        c = uart_getc();
    return c;
}

// Blocking line editor. Reads into buf (at most maxlen-1 chars), echoing to
// both the screen and UART, handling backspace, and blinking the cursor while
// it waits. The trailing newline is consumed but not stored. Returns length.
int term_getline(char *buf, int maxlen) {
    int n = 0;
    uint32_t last_blink = TIMER_CLO;
    for (;;) {
        char c = term_pollchar();
        if (c == '\r' || c == '\n') {
            term_putchar('\n');
            uart_puts("\r\n");
            buf[n] = 0;
            return n;
        } else if (c == '\b') {
            if (n > 0) {
                n--;
                term_putchar('\b');          // erases the cell on screen
                uart_puts("\b \b");          // erase on a real terminal
            }
        } else if (c >= 32 && c <= 126) {
            if (c >= 'a' && c <= 'z') c -= 32;    // CAPS LOCK on, like a real BBC Micro
            if (n < maxlen - 1) {
                buf[n++] = c;
                term_putchar(c);
                uart_putc(c);
            }
        }

        // Keep the cursor blinking on a fixed wall-clock interval while idle.
        uint32_t now = TIMER_CLO;
        if (now - last_blink >= CURSOR_BLINK_US) {
            last_blink = now;
            cursor_set(!cursor_visible);
        }
    }
}

// ---------------------------------------------------------------------------
// Console backend for the BASIC interpreter (console.h). Output is mirrored to
// both the framebuffer terminal and the UART so headless/serial use works too.
// ---------------------------------------------------------------------------

void con_putc(char c) {
    if (c == '\n') { term_putchar('\n'); uart_puts("\r\n"); }
    else           { term_putchar(c);    uart_putc(c); }
}

void con_puts(const char *s) {
    while (*s) con_putc(*s++);
}

int con_getline(char *buf, int maxlen) {
    return term_getline(buf, maxlen);   // never EOFs on the target
}

void con_cls(void) {
    if (fb_ready) cleardevice();
    cursor_col = 0;
    cursor_row = 0;
    cursor_visible = 0;
    uart_puts("\r\n");
}

void con_colour(int c) {
    text_color = bbc_palette[c & 7];    // BBC foreground COLOUR 0..7
}

// Block until a key is pressed; return its ASCII code (BBC GET).
int con_getkey(void) {
    uint32_t last_blink = TIMER_CLO;
    for (;;) {
        char c = term_pollchar();
        if (c) { if (cursor_visible) cursor_set(0); return (unsigned char)c; }
        uint32_t now = TIMER_CLO;
        if (now - last_blink >= CURSOR_BLINK_US) {
            last_blink = now;
            cursor_set(!cursor_visible);
        }
    }
}

#define TIMER_CHI (*(volatile uint32_t *)(0xFE003008UL))

unsigned long long con_micros(void) {
    uint32_t hi, lo;
    do { hi = TIMER_CHI; lo = TIMER_CLO; } while (hi != TIMER_CHI);  // guard wrap
    return ((unsigned long long)hi << 32) | lo;
}

// Wait up to `centiseconds` (1/100 s) for a key; return its code or -1.
int con_inkey(int centiseconds) {
    uint32_t t0 = TIMER_CLO;
    uint32_t limit = (centiseconds > 0) ? (uint32_t)centiseconds * 10000u : 0;
    uint32_t last_blink = TIMER_CLO;
    for (;;) {
        char c = term_pollchar();
        if (c) { if (cursor_visible) cursor_set(0); return (unsigned char)c; }
        if (TIMER_CLO - t0 >= limit) return -1;
        uint32_t now = TIMER_CLO;
        if (now - last_blink >= CURSOR_BLINK_US) {
            last_blink = now;
            cursor_set(!cursor_visible);
        }
    }
}

int con_pos(void)  { return cursor_col; }
int con_vpos(void) { return cursor_row; }

// ---------------------------------------------------------------------------
// Boot logo (embedded RGB image from logo_data.c)
// ---------------------------------------------------------------------------

extern const unsigned int  logo_w;
extern const unsigned int  logo_h;
extern const unsigned char logo_rgb[];

static int g_show_logo = 0;   // set at boot once the framebuffer is up

// Draw the embedded RGB logo at framebuffer pixel (ox, oy), shrunk by integer
// factor `div` (nearest-neighbour). Colours are already baked into logo_rgb
// (black background, green leaves, raspberry-red berry). The framebuffer is
// RGBA byte-order (low byte = R), so a pixel is 0xFF000000 | B<<16 | G<<8 | R.
static void draw_logo(int ox, int oy, int div) {
    unsigned int dw = logo_w / div, dh = logo_h / div;
    for (unsigned int y = 0; y < dh; y++) {
        for (unsigned int x = 0; x < dw; x++) {
            const unsigned char *p = logo_rgb + ((y * div) * logo_w + (x * div)) * 3;
            uint32_t r = p[0], g = p[1], b = p[2];
            putpixel(ox + (int)x, oy + (int)y,
                     0xFF000000u | (b << 16) | (g << 8) | r);
        }
    }
}

// Show the boot splash. On the target this clears the boot-progress text, paints
// the logo on the left, prints the banner beside it (vertically centred on the
// logo), parks the text cursor cleanly below the logo, and returns 1 so the
// caller does not print the banner again. Returns 0 when no logo is shown (e.g.
// the host build), in which case the caller prints the banner itself.
int con_splash(const char *banner) {
    if (!g_show_logo || !fb_ready) return 0;
    cleardevice();                                    // wipe the boot-progress text
    int div = 2;                                      // half size
    int lw = (int)logo_w / div, lh = (int)logo_h / div;
    int ox = CHAR_W;                                  // small left margin
    int oy = CHAR_H;                                  // small top margin
    draw_logo(ox, oy, div);                           // left side, half size

    // Banner beside the logo, vertically centred on it.
    cursor_col = (ox + lw) / CHAR_W + 1;
    cursor_row = (oy + lh / 2) / CHAR_H;
    cursor_visible = 0;
    con_puts(banner);
    uart_puts("\r\n");                                // tidy the serial line

    // Park the cursor below the logo for the REPL prompt.
    cursor_col = 0;
    cursor_row = (oy + lh) / CHAR_H + 1;
    cursor_visible = 0;
    return 1;
}

// ---------------------------------------------------------------------------
// BBC-style graphics
//
// Logical coordinate space is 1280x1024 with the origin at the bottom-left,
// mapped onto the physical framebuffer. State: graphics foreground/background
// colours, the GCOL plot action, and a short history of the last three points
// (PLOT lines use the last two, triangles the last three, circles use the last
// two as centre + a point on the circumference).
// ---------------------------------------------------------------------------

#define GFX_LW 1280
#define GFX_LH 1024

static int gfx_fg  = 7;     // graphics foreground (logical colour)
static int gfx_bg  = 0;     // graphics background
static int gfx_act = 0;     // GCOL plot action (0=store 1=OR 2=AND 3=EOR 4=invert)
static int gpx0, gpy0;      // most recent point (logical)
static int gpx1, gpy1;      // previous point
static int gpx2, gpy2;      // the one before that

static int lx2px(int x) { return x * FB_WIDTH  / GFX_LW; }
static int ly2py(int y) { return (FB_HEIGHT - 1) - (y * FB_HEIGHT / GFX_LH); }

static int isqrt_i(int v) {
    if (v <= 0) return 0;
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

void con_mode(int n) {
    (void)n;                                  // resolution is fixed; MODE just resets
    if (fb_ready) cleardevice();
    cursor_col = 0; cursor_row = 0; cursor_visible = 0;
    text_color = bbc_palette[7];
    gfx_fg = 7; gfx_bg = 0; gfx_act = 0;
    gpx0 = gpy0 = gpx1 = gpy1 = gpx2 = gpy2 = 0;
    gfx_set_op(0);
    uart_puts("\r\n");
}

void con_gcol(int action, int colour) {
    gfx_act = action & 7;
    if (colour >= 128) gfx_bg = (colour - 128) & 7;   // GCOL a,128+c sets background
    else               gfx_fg = colour & 7;
}

void con_clg(void) {
    if (!fb_ready) return;
    gfx_set_op(0);
    fill_rect(0, 0, FB_WIDTH - 1, FB_HEIGHT - 1, bbc_palette[gfx_bg & 7]);
}

void con_plot(int code, int x, int y) {
    if (!fb_ready) return;
    int absolute = code & 4;
    int cmode    = code & 3;        // 0=move 1=foreground 2=inverse 3=background
    int group    = code & 0xF8;

    int nx, ny;
    if (absolute) { nx = x;        ny = y;        }
    else          { nx = gpx0 + x; ny = gpy0 + y; }

    gpx2 = gpx1; gpy2 = gpy1;       // shift point history
    gpx1 = gpx0; gpy1 = gpy0;
    gpx0 = nx;   gpy0 = ny;

    if (group == 0 && cmode == 0) return;          // plain MOVE

    uint32_t col = (cmode == 3) ? bbc_palette[gfx_bg & 7] : bbc_palette[gfx_fg & 7];
    int op = (cmode == 2) ? 3 : gfx_act;           // inverse colour -> EOR
    gfx_set_op(op);

    int x0 = lx2px(gpx0), y0 = ly2py(gpy0);
    int x1 = lx2px(gpx1), y1 = ly2py(gpy1);
    int x2 = lx2px(gpx2), y2 = ly2py(gpy2);

    switch (group) {
        case 0:    draw_line(x1, y1, x0, y0, col); break;          // line
        case 64:   draw_line(x0, y0, x0, y0, col); break;          // single point
        case 80:   fill_triangle(x0, y0, x1, y1, x2, y2, col); break;   // triangle
        case 96:   fill_rect(x1, y1, x0, y0, col); break;          // rectangle
        case 144:                                                   // circle outline
        case 152: {                                                 // circle fill
            int dx = x0 - x1, dy = y0 - y1;
            int r  = isqrt_i(dx * dx + dy * dy);
            if (group == 152) fill_circle(x1, y1, r, col);
            else              draw_circle(x1, y1, r, col);
            break;
        }
        default:   draw_line(x1, y1, x0, y0, col); break;          // fallback: line
    }
    gfx_set_op(0);                                  // leave op at store
}

int con_point(int x, int y) {
    if (!fb_ready) return -1;
    if (x < 0 || x >= GFX_LW || y < 0 || y >= GFX_LH) return -1;
    uint32_t px = getpixel(lx2px(x), ly2py(y));
    for (int i = 0; i < 8; i++) if (bbc_palette[i] == px) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Exception handler (called from the vector table in vectors.S)
// ---------------------------------------------------------------------------

// Print a labelled hex value straight to the framebuffer terminal (used by the
// exception handler so fault details are readable on screen even when the SD
// log write itself fails).
static void term_hexval(const char *label, uint64_t v, int digits) {
    term_puts(label);
    const char *h = "0123456789ABCDEF";
    for (int i = (digits - 1) * 4; i >= 0; i -= 4)
        term_putchar(h[(v >> i) & 0xF]);
    term_putchar('\n');
}

// Print the last `max_lines` lines of the in-RAM boot log to the framebuffer.
// Lets the exception handler show how far boot got (e.g. the [PCIE]/[XHCI]
// progress lines) on screen, without needing the SD card.
static void term_log_tail(int max_lines) {
    const char *buf; int len;
    uart_log_get(&buf, &len);
    int start = 0, nl = 0;
    for (int i = len - 1; i >= 0; i--) {
        if (buf[i] == '\n') { if (++nl > max_lines) { start = i + 1; break; } }
    }
    for (int i = start; i < len; i++) term_putchar(buf[i]);
}

void exception_handler(uint64_t type, uint64_t esr, uint64_t elr, uint64_t far) {
    static const char *vec_names[16] = {
        "SYNC/SP0",  "IRQ/SP0",  "FIQ/SP0",  "SERROR/SP0",
        "SYNC",      "IRQ",      "FIQ",       "SERROR",
        "SYNC/L64",  "IRQ/L64",  "FIQ/L64",   "SERROR/L64",
        "SYNC/L32",  "IRQ/L32",  "FIQ/L32",   "SERROR/L32",
    };
    uint32_t ec = (uint32_t)(esr >> 26) & 0x3F;
    const char *cause = "unknown";
    switch (ec) {
        case 0x00: cause = "unknown reason";       break;
        case 0x20: case 0x21: cause = "instruction abort"; break;
        case 0x22: cause = "PC alignment fault";   break;
        case 0x24: case 0x25: cause = "data abort"; break;
        case 0x26: cause = "SP alignment fault";   break;
        case 0x3C: cause = "BRK instruction";      break;
    }

    uart_puts("\n\n*** UNHANDLED EXCEPTION ***\n");
    uart_puts("vector : "); uart_puts(vec_names[type & 15]); uart_putc('\n');
    uart_puts("cause  : "); uart_puts(cause);                uart_putc('\n');
    uart_hex  ("ESR_EL1: ", (uint32_t)esr);
    uart_hex  ("  EC   : ", ec);
    uart_hex64("ELR_EL1: ", elr);   // faulting instruction
    uart_hex64("FAR_EL1: ", far);   // faulting address
    uart_puts("system halted.\n");

    // Paint the fault details straight onto the screen FIRST - this is just
    // framebuffer memory writes, so it works even when the SD/FAT path that
    // flush_boot_log() uses is itself wedged by the fault (e.g. a pending PCIe
    // SError that re-fires during the SD write). Read these off the screen.
    if (fb_ready) {
        fill_rect(0, 0, FB_WIDTH - 1, FB_HEIGHT - 1, COLOR_BLACK);
        cursor_col = 0; cursor_row = 0; cursor_visible = 0;
        text_color = COLOR_WHITE;
        // Recent boot progress first (shows the [PCIE]/[XHCI] lines), then the
        // fault details at the bottom so they stay visible if the log scrolls.
        term_puts("--- recent log ---\n");
        term_log_tail(20);
        term_puts("*** EXCEPTION ***\n");
        term_puts("vector: "); term_puts(vec_names[type & 15]); term_putchar('\n');
        term_puts("cause : "); term_puts(cause); term_putchar('\n');
        term_hexval("ESR = ", esr, 8);
        term_hexval("ELR = ", elr, 16);
        term_hexval("FAR = ", far, 16);
    }

    // Then also try to dump the full log to the SD card (best-effort).
    flush_boot_log();
    // vectors.S halts the CPU after we return.
}

// ---------------------------------------------------------------------------
// Board identification
// ---------------------------------------------------------------------------

// Query and log the board serial (tag 0x00010004). Real Pi hardware returns a
// nonzero serial; QEMU's raspi4b leaves it 0. Returns 1 on real hardware. We use
// this to avoid touching the PCIe controller under QEMU, which does not model it
// (an access there raises an external abort).
static int board_real_hw(void) {
    mbox[0] = 9 * 4;
    mbox[1] = 0;
    mbox[2] = 0x00010004; mbox[3] = 8; mbox[4] = 0;   // get board serial
    mbox[5] = 0; mbox[6] = 0;
    mbox[7] = 0;                                       // end tag
    mbox[8] = 0;
    mbox_call();
    uart_hex("[BOARD] serial lo: ", mbox[5]);
    uart_hex("[BOARD] serial hi: ", mbox[6]);
    return (mbox[5] | mbox[6]) != 0;
}

// ---------------------------------------------------------------------------
// Framebuffer setup via mailbox
// ---------------------------------------------------------------------------

static int setup_fb(void) {
    mbox[ 0] = 35 * 4;
    mbox[ 1] = 0;
    mbox[ 2] = 0x00048003; mbox[ 3] = 8; mbox[ 4] = 8;   // set physical w/h
    mbox[ 5] = FB_WIDTH;
    mbox[ 6] = FB_HEIGHT;
    mbox[ 7] = 0x00048004; mbox[ 8] = 8; mbox[ 9] = 8;   // set virtual w/h
    mbox[10] = FB_WIDTH;
    mbox[11] = FB_HEIGHT;
    mbox[12] = 0x00048005; mbox[13] = 4; mbox[14] = 4;   // set depth
    mbox[15] = 32;
    // Force pixel order to RGB (1). Without this each platform uses its own
    // default - QEMU is RGB but the real Pi 4 firmware defaults to BGR, which
    // swaps red/blue and turns the raspberry logo purple. We pack pixels with red
    // in the low byte, which is the RGB order.
    mbox[16] = 0x00048006; mbox[17] = 4; mbox[18] = 4;   // set pixel order
    mbox[19] = 1;                                        // 1 = RGB
    mbox[20] = 0x00040001; mbox[21] = 8; mbox[22] = 8;   // allocate framebuffer
    mbox[23] = 4096;                                     // -> base address
    mbox[24] = 0;                                        // -> size
    mbox[25] = 0x00040008; mbox[26] = 4; mbox[27] = 4;   // get pitch
    mbox[28] = 0;                                        // -> pitch
    mbox[29] = 0;                                        // end tag

    int ok = mbox_call();
    uart_dec("[FB] mbox_call returned: ", (uint32_t)ok);
    uart_hex("[FB] mbox[1] response:   ", mbox[1]);
    uart_dec("[FB] width:  ", mbox[5]);
    uart_dec("[FB] height: ", mbox[6]);
    uart_hex("[FB] pixel order: ", mbox[19]);
    uart_hex("[FB] bus addr: ", mbox[23]);
    uart_dec("[FB] size:   ", mbox[24]);
    uart_dec("[FB] pitch:  ", mbox[28]);

    if (!ok || mbox[28] == 0) return 0;

    uint32_t fb_bus  = mbox[23];
    uint32_t fb_phys = fb_bus & 0x3FFFFFFF;  // strip VideoCore bus alias
    uint32_t pitch   = mbox[28];
    uint32_t fb_size = mbox[24];
    uint32_t *fb     = (uint32_t *)(uintptr_t)fb_phys;

    uart_hex("[FB] physical: ", fb_phys);
    // The framebuffer is brought up before the MMU (see kernel_main), so we just
    // record its extent here; kernel_main re-marks it non-cacheable once the MMU
    // page tables exist, so CPU pixel writes stay coherent with the GPU scan-out.
    g_fb_phys = fb_phys;
    g_fb_size = fb_size;
    init_graphics(fb, mbox[5], mbox[6], pitch);
    fb_ready = 1;
    return 1;
}

// ---------------------------------------------------------------------------
// Kernel entry
// ---------------------------------------------------------------------------

void kernel_main(void) {
    uart_init();
    uart_puts("\n[BOOT] RPi4 bare metal kernel started\n");

    // Bring the framebuffer up BEFORE enabling the MMU. With caches off the
    // framebuffer is naturally coherent with the GPU, and - crucially - this
    // gives us a screen to print on even if the MMU/cache bring-up is itself what
    // hangs on real silicon. boot_msg() mirrors progress to this screen once up,
    // so the last line you see is exactly where the kernel stopped.
    uart_puts("[FB] setting up framebuffer...\n");
    if (!setup_fb()) {
        uart_puts("[FB] ERROR: framebuffer setup failed\n");
    } else {
        cleardevice();
        boot_msg("[FB] framebuffer OK\n");
    }

    // Enable MMU + caches (big speed-up; also lets unaligned access work). On
    // real hardware this invalidates stale firmware cache state first.
    boot_msg("[MMU] enabling MMU + caches...\n");
    mmu_init();
    mmu_set_noncached(g_fb_phys, g_fb_size);   // re-mark FB NC (mmu_init rebuilt the tables)
    boot_msg("[MMU] enabled\n");

    // Show the BerryBasic boot logo (con_splash draws it when the REPL banner
    // prints; clearing the boot-progress text first).
    int real_hw = board_real_hw();
    g_show_logo = 1;

    // Mount the SD card (the data partition holds the user's BASIC programs).
    boot_msg("[SD] mounting filesystem...\n");
    g_sd_ready = (stg_init() == 0);
    boot_msg(g_sd_ready ? "[SD] mounted\n" : "[SD] mount FAILED\n");

    // USB keyboard on the DWC2 OTG / USB-C port (this is what QEMU emulates).
    // NOTE: on a board whose only USB-C port is power, no device is present here;
    // if boot visibly stops on the next line, the DWC2 probe is the culprit.
    boot_msg("[USB] initialising keyboard (DWC2/USB-C)...\n");
    g_kbd_ok = usb_kbd_init();
    boot_msg(g_kbd_ok ? "[USB] keyboard ready\n"
                      : "[USB] no USB-C keyboard\n");

    // On real hardware the USB-A ports are behind the VL805 xHCI on PCIe; bring
    // it up and look for a keyboard there too. QEMU does not model the PCIe
    // controller (touching it faults), so only do this on real hardware.
    if (!g_kbd_ok && real_hw) {
        boot_msg("[USB] trying USB-A (PCIe/VL805 xHCI)...\n");
        uintptr_t xhci_mmio = pcie_init();
        if (xhci_mmio) g_xhci_ok = xhci_kbd_init(xhci_mmio);
        boot_msg(g_xhci_ok ? "[USB] USB-A keyboard ready\n"
                           : "[USB] no USB-A keyboard\n");
    }

    // The boot log is only written to the card on a crash (see exception_handler),
    // so normal boots leave the data partition holding nothing but user programs.

    // Hand control to the BASIC interpreter (runs forever on the target).
    boot_msg("[LOOP] entering BASIC\n");
    basic_init();
    basic_repl();
}
