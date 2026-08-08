// INPUTLOG.SED - a live input monitor: watches the keyboard and the mouse and
// draws what it sees on the screen.
//
//   INPUTLOG                 ' panel near the top-left
//   INPUTLOG 300, 200        ' panel at your own position
//
// Shows, updating as you type and move:
//   * the pointer position, and how far it moved since the last sample
//   * which buttons are down, as three indicator lamps
//   * the last key, as a character and as a code
//   * running totals of keys, clicks and moves
//   * a log of the last few discrete events (keys and clicks)
//
// Press Q or ESC to return to BASIC.
//
// Three things about input from a seed are worth reading the loop for:
//
//  * The mouse call IS the poll. The interpreter services the pointer between
//    BASIC statements, and while a seed runs there are no BASIC statements - so
//    nothing else is reading the mouse. This loop polls it itself.
//
//  * svc->inkey(0) is the non-blocking read: -1 straight away when no key is
//    waiting, so the loop keeps sampling the mouse. svc->getkey() would block
//    and the mouse would stop being read for as long as it blocked.
//
//  * The system's arrow is hidden for the duration (inkey hides it whenever it
//    returns), so the seed draws its own crosshair. That is also why the seed
//    owns the screen here: it clears it on the way in and out.
//
// The panel is redrawn by clearing it and writing the text back, which on a
// live screen would be seen as a flicker: the display can scan out the gap
// between the two. So the loop draws off-screen and shows each finished frame
// in one go - setdoublebuffer(1) + flippage(). The buffer keeps its contents
// between flips, so the redraw can still be incremental.
//
// Mouse coordinates are raw framebuffer pixels - the same space this file's
// drawing calls use - so a position can be plotted directly.
#include "seed.h"
#include <graphics.h>
#include <stdio.h>
#include <string.h>

#define PW      430               // panel size
#define PH      390
#define LINE     22               // text line height
#define TEXTSZ   17
#define LOG_N     6               // remembered events

#define BTN_L  1                  // svc->mouse button bitmask
#define BTN_R  2
#define BTN_M  4

// Name a key for display. Printable keys speak for themselves; the rest arrive
// as SEED_KEY_* codes, which is the whole point of the key path being int-wide:
// F5 is 0x104, well clear of the Latin-1 characters (æ ø å …) that live at 0x80
// and above. Returns 0 for a key that has no name, meaning "print it as text".
static const char *key_name(int k) {
    switch (k) {
        case SEED_KEY_LEFT:  return "Left";
        case SEED_KEY_RIGHT: return "Right";
        case SEED_KEY_UP:    return "Up";
        case SEED_KEY_DOWN:  return "Down";
        case SEED_KEY_HOME:  return "Home";
        case SEED_KEY_END:   return "End";
        case SEED_KEY_INS:   return "Insert";
        case SEED_KEY_DEL:   return "Delete";
        case SEED_KEY_ESC:   return "Esc";
        case SEED_KEY_PGUP:  return "PgUp";
        case SEED_KEY_PGDN:  return "PgDn";
        case '\n':           return "Enter";
        case '\t':           return "Tab";
        case '\b':           return "Backspace";
        case ' ':            return "Space";
        default:             return 0;
    }
}

// The event log holds text, not pointers: an array of `char *` would need its
// addresses baked into the blob, which is exactly what a seed may not have (see
// "Splitting a seed across several files" in the manual).
static char log_txt[LOG_N][40];
static int  log_n;

// Describe a key: "F5", "Esc", "'a'", or "F12" - into `out`, which must hold 16.
static void key_text(int k, char *out, int outsz) {
    const char *nm = key_name(k);
    if (nm) { snprintf(out, outsz, "%s", nm); return; }
    if (k >= SEED_KEY_F1 && k <= SEED_KEY_F12) {
        snprintf(out, outsz, "F%d", k - SEED_KEY_F1 + 1);
        return;
    }
    if (k >= 32 && k < 127) { snprintf(out, outsz, "'%c'", (char)k); return; }
    snprintf(out, outsz, "chr %d", k);           // Latin-1, or something exotic
}

static void log_add(const char *s) {
    if (log_n < LOG_N) {                        // still filling up
        strncpy(log_txt[log_n], s, sizeof log_txt[0] - 1);
        log_txt[log_n][sizeof log_txt[0] - 1] = 0;
        log_n++;
        return;
    }
    for (int i = 0; i < LOG_N - 1; i++)         // full: scroll the oldest out
        memcpy(log_txt[i], log_txt[i + 1], sizeof log_txt[0]);
    strncpy(log_txt[LOG_N - 1], s, sizeof log_txt[0] - 1);
    log_txt[LOG_N - 1][sizeof log_txt[0] - 1] = 0;
}

// One indicator lamp: filled when it is on. `w` is the box width - wide enough
// for the label, which the text is centred in.
static void lamp(int x, int y, int w, int on, const char *label) {
    setfillstyle(SOLID_FILL, on ? LIGHTGREEN : DARKGRAY);
    bar(x, y - 12, x + w, y + 2);
    setcolor(WHITE);
    rectangle(x, y - 12, x + w, y + 2);
    setcolor(on ? BLACK : LIGHTGRAY);
    int tw = textwidth(label);
    outtextxy(x + (w - tw) / 2, y - 12, label);
}

static void draw_panel(int px, int py, int mx, int my, int dx, int dy, int b,
                       int key, int mods, long keys, long clicks, long moves) {
    char s[64];

    setfillstyle(SOLID_FILL, BLUE);             // panel body + border
    bar(px, py, px + PW, py + PH);
    setcolor(WHITE);
    rectangle(px, py, px + PW, py + PH);

    int x = px + 14, y = py + 10;
    setcolor(YELLOW);
    outtextxy(x, y, "INPUT MONITOR");
    setcolor(LIGHTGRAY);
    outtextxy(px + PW - 130, y, "Q / ESC = quit");
    y += LINE + 6;

    setcolor(WHITE);
    snprintf(s, sizeof s, "mouse    x =%5d   y =%5d", mx, my);
    outtextxy(x, y, s); y += LINE;
    snprintf(s, sizeof s, "moved   dx =%+5d  dy =%+5d", dx, dy);
    outtextxy(x, y, s); y += LINE;

    outtextxy(x, y, "buttons");
    lamp(x + 110, y + 14, 26, b & BTN_L, "L");
    lamp(x + 145, y + 14, 26, b & BTN_R, "R");
    lamp(x + 180, y + 14, 26, b & BTN_M, "M");
    y += LINE;

    if (key >= 0) {
        char kt[16];
        key_text(key, kt, sizeof kt);
        snprintf(s, sizeof s, "last key %-10s code %d", kt, key);
    } else {
        snprintf(s, sizeof s, "last key  -");
    }
    outtextxy(x, y, s); y += LINE;

    // Modifiers and locks are state, not keys: they never come back from
    // inkey(), so they are polled separately with svc->keymods().
    outtextxy(x, y, "held");
    lamp(x + 110, y + 14, 34, mods & SEED_KMOD_SHIFT, "Sh");
    lamp(x + 152, y + 14, 34, mods & SEED_KMOD_CTRL,  "Ct");
    lamp(x + 194, y + 14, 34, mods & SEED_KMOD_ALT,   "Alt");
    lamp(x + 236, y + 14, 34, mods & SEED_KMOD_ALTGR, "Gr");
    lamp(x + 278, y + 14, 34, mods & SEED_KMOD_META,  "Me");
    y += LINE;

    outtextxy(x, y, "locks");
    lamp(x + 110, y + 14, 42, mods & SEED_KMOD_CAPS,   "Caps");
    lamp(x + 160, y + 14, 42, mods & SEED_KMOD_NUM,    "Num");
    lamp(x + 210, y + 14, 42, mods & SEED_KMOD_SCROLL, "Scrl");
    y += LINE;

    snprintf(s, sizeof s, "keys %ld   clicks %ld   moves %ld", keys, clicks, moves);
    outtextxy(x, y, s); y += LINE + 6;

    setcolor(LIGHTCYAN);
    outtextxy(x, y, "recent events");
    y += LINE;
    setcolor(LIGHTGRAY);
    for (int i = log_n - 1; i >= 0; i--) {      // newest first
        outtextxy(x + 10, y, log_txt[i]);
        y += LINE;
    }
}

static void crosshair(int x, int y, int colour) {
    setcolor(colour);
    line(x - 9, y, x + 9, y);
    line(x, y - 9, x, y + 9);
}

SEED_KEYWORD("INPUTLOG", SEED_KW_STATEMENT, 0, 2) {
    if (!initgraph()) return 0;                 // no framebuffer (host build)

    int font = loadfont("PHILO.TTF");
    if (!font) {
        const char *m = "INPUTLOG needs PHILO.TTF on the card\n";
        svc->puts(m, (int)strlen(m));
        return 0;
    }
    settextfont(font);
    settextsize(TEXTSZ);
    settextjustify(LEFT_TEXT, TOP_TEXT);

    int px = 40, py = 40;                       // panel position
    if (argc >= 1) px = (int)argv[0].num;
    if (argc >= 2) py = (int)argv[1].num;

    log_n = 0;
    long keys = 0, clicks = 0, moves = 0;
    int  key = -1;
    int  mods = 0;                              // modifier/lock snapshot
    int  ox = -1, oy = -1;                      // previous crosshair (-1 = none yet)
    int  mx = 0, my = 0, b = 0;                 // last sampled pointer state
    int  dx = 0, dy = 0;
    char s[40];

    // Draw off-screen so a half-redrawn panel is never shown. The setting is
    // shared with BASIC's BUFFER, so remember how we found it and put it back.
    int was_buffered = getdoublebuffer();
    setdoublebuffer(1);                         // <0 = no buffer; we just flicker

    setbkcolor(BLACK);
    cleardevice();
    svc->mouse(&mx, &my, &b);                   // baseline, so the first frame is calm
    int dirty = 1;

    for (;;) {
        int k = svc->inkey(0);                  // non-blocking: -1 when nothing waiting
        if (k == 'q' || k == 'Q' || k == SEED_KEY_ESC) break;
        if (k > 0) {
            key = k;
            keys++;
            char kt[16];
            key_text(k, kt, sizeof kt);
            snprintf(s, sizeof s, "key  %-10s %d", kt, k);
            log_add(s);
            dirty = 1;
        }

        // Modifiers/locks are separate state - inkey() above is what refreshes
        // it, so this must come after the read, not before.
        int nmods = svc->keymods();
        if (nmods != mods) { mods = nmods; dirty = 1; }

        int nx, ny, nb;
        svc->mouse(&nx, &ny, &nb);              // this call polls the hardware

        if (nx != mx || ny != my) {
            dx = nx - mx; dy = ny - my;
            mx = nx; my = ny;
            moves++;
            dirty = 1;
        }
        if (nb != b) {
            int pressed = nb & ~b;              // 0 -> 1 edges only: one click each
            if (pressed) {
                clicks++;
                char which = (pressed & BTN_L) ? 'L' : (pressed & BTN_R) ? 'R' : 'M';
                snprintf(s, sizeof s, "click %c  @ %d,%d", which, mx, my);
                log_add(s);
            }
            b = nb;
            dirty = 1;
        }

        if (!dirty) continue;                   // nothing changed: leave the screen alone
        dirty = 0;

        if (ox >= 0) crosshair(ox, oy, BLACK);  // rub out the old crosshair,
        draw_panel(px, py, mx, my, dx, dy, b, key, mods, keys, clicks, moves);
        crosshair(mx, my, YELLOW);              // then draw the new one on top
        ox = mx; oy = my;
        flippage();                             // ... and show the frame, all at once
    }

    cleardevice();                              // hand a clean screen back to BASIC
    flippage();
    setdoublebuffer(was_buffered);              // leave BASIC's setting as we found it
    closegraph();
    snprintf(s, sizeof s, "keys %ld  clicks %ld  moves %ld\n", keys, clicks, moves);
    svc->puts(s, (int)strlen(s));
    return 0;
}
