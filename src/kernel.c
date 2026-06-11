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

// Dump the in-RAM UART log to BOOTLOG.TXT on the SD card. Best-effort: needs the
// filesystem mounted, and guards against re-entry (e.g. from an exception while
// already writing). Lets a board with no serial cable still surface diagnostics.
static void flush_boot_log(void) {
    static int busy = 0;
    if (!g_sd_ready || busy) return;
    busy = 1;
    const char *buf; int len;
    uart_log_get(&buf, &len);
    stg_write("BOOTLOG.TXT", buf, len);
    busy = 0;
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

static int g_show_logo = 0;   // set at boot when running under QEMU

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

// Show the boot splash. On the target (QEMU session only) this paints the
// inverted logo on the left, prints the banner beside it (vertically centred on
// the logo), parks the text cursor cleanly below the logo, and returns 1 so the
// caller does not print the banner again. Returns 0 when no logo is shown, in
// which case the caller prints the banner itself (host, or non-QEMU target).
int con_splash(const char *banner) {
    if (!g_show_logo || !fb_ready) return 0;
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

    // Best-effort: dump the log (including this fault) to the SD card so it can
    // be read without a serial cable.
    flush_boot_log();

    if (fb_ready) term_puts("\n*** EXCEPTION - see BOOTLOG.TXT on the SD card ***\n");
    // vectors.S halts the CPU after we return.
}

// ---------------------------------------------------------------------------
// Board identification (used to tell QEMU apart from real hardware)
// ---------------------------------------------------------------------------

// Query board revision (tag 0x00010002) and serial (0x00010004). On real Pi
// hardware these are nonzero; QEMU's raspi4b leaves the serial as 0. We treat
// "serial == 0" as "running under QEMU" so we can show the boot logo there.
static int running_under_qemu(void) {
    mbox[0] = 9 * 4;
    mbox[1] = 0;
    mbox[2] = 0x00010004; mbox[3] = 8; mbox[4] = 0;   // get board serial
    mbox[5] = 0; mbox[6] = 0;
    mbox[7] = 0;                                       // end tag
    mbox[8] = 0;
    int ok = mbox_call();
    uint32_t lo = mbox[5], hi = mbox[6];
    uart_hex("[BOARD] serial lo: ", lo);
    uart_hex("[BOARD] serial hi: ", hi);
    return ok && lo == 0 && hi == 0;
}

// ---------------------------------------------------------------------------
// Framebuffer setup via mailbox
// ---------------------------------------------------------------------------

static int setup_fb(void) {
    mbox[ 0] = 35 * 4;
    mbox[ 1] = 0;
    mbox[ 2] = 0x00048003; mbox[ 3] = 8; mbox[ 4] = 8;
    mbox[ 5] = FB_WIDTH;
    mbox[ 6] = FB_HEIGHT;
    mbox[ 7] = 0x00048004; mbox[ 8] = 8; mbox[ 9] = 8;
    mbox[10] = FB_WIDTH;
    mbox[11] = FB_HEIGHT;
    mbox[12] = 0x00048005; mbox[13] = 4; mbox[14] = 4;
    mbox[15] = 32;
    mbox[16] = 0x00040001; mbox[17] = 8; mbox[18] = 8;
    mbox[19] = 4096;
    mbox[20] = 0;
    mbox[21] = 0x00040008; mbox[22] = 4; mbox[23] = 4;
    mbox[24] = 0;
    mbox[25] = 0;

    int ok = mbox_call();
    uart_dec("[FB] mbox_call returned: ", (uint32_t)ok);
    uart_hex("[FB] mbox[1] response:   ", mbox[1]);
    uart_dec("[FB] width:  ", mbox[5]);
    uart_dec("[FB] height: ", mbox[6]);
    uart_hex("[FB] bus addr: ", mbox[19]);
    uart_dec("[FB] size:   ", mbox[20]);
    uart_dec("[FB] pitch:  ", mbox[24]);

    if (!ok || mbox[24] == 0) return 0;

    uint32_t fb_bus  = mbox[19];
    uint32_t fb_phys = fb_bus & 0x3FFFFFFF;  // strip VideoCore bus alias
    uint32_t pitch   = mbox[24];
    uint32_t fb_size = mbox[20];
    uint32_t *fb     = (uint32_t *)(uintptr_t)fb_phys;

    uart_hex("[FB] physical: ", fb_phys);
    // On real hardware the GPU scans the framebuffer straight out of RAM, so map
    // it non-cacheable to keep CPU pixel writes coherent (no-op effect in QEMU).
    mmu_set_noncached(fb_phys, fb_size);
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

    // Enable MMU + caches early (big speed-up; also lets unaligned access work).
    mmu_init();
    uart_puts("[MMU] enabled (MMU + D/I caches)\n");

    // Framebuffer (the BBC boot banner is printed by basic_repl)
    uart_puts("[FB] setting up framebuffer...\n");
    if (!setup_fb()) {
        uart_puts("[FB] ERROR: framebuffer setup failed\n");
    } else {
        uart_puts("[FB] framebuffer OK\n");
        cleardevice();
    }

    // Decide whether to show the boot logo (only inside a QEMU session).
    g_show_logo = running_under_qemu();
    uart_dec("[BOARD] qemu session: ", (uint32_t)g_show_logo);

    // Mount the SD card FIRST, so the boot log can be dumped to it (the only way
    // to read diagnostics on a board with no serial cable).
    uart_puts("[SD] mounting filesystem...\n");
    g_sd_ready = (stg_init() == 0);

    // USB keyboard on the DWC2 OTG / USB-C port (this is what QEMU emulates).
    uart_puts("[USB] initialising keyboard...\n");
    g_kbd_ok = usb_kbd_init();
    uart_puts(g_kbd_ok ? "[USB] keyboard ready\n"
                       : "[USB] no USB keyboard - UART input only\n");
    flush_boot_log();                  // checkpoint: boot + SD + DWC2 captured

    // On real hardware the USB-A ports are behind the VL805 xHCI on PCIe; bring
    // it up and look for a keyboard there too. (No PCIe under QEMU -> skipped.)
    if (!g_kbd_ok) {
        uart_puts("[USB] trying USB-A (PCIe/VL805 xHCI)...\n");
        uintptr_t xhci_mmio = pcie_init();
        if (xhci_mmio) g_xhci_ok = xhci_kbd_init(xhci_mmio);
        uart_puts(g_xhci_ok ? "[USB] USB-A keyboard ready\n"
                            : "[USB] no USB-A keyboard\n");
        flush_boot_log();              // full log including the USB-A bring-up
    }

    // Hand control to the BASIC interpreter (runs forever on the target).
    uart_puts("[LOOP] entering BASIC\n");
    basic_init();
    basic_repl();
}
