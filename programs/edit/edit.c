/* ===========================================================================
 *  ed - a small full-screen text editor for BerryBasiC
 *
 *  Shipped to /sys as the `ed` command (EDIT is a reserved keyword - the
 *  built-in line editor - so this file editor uses the classic short name).
 *
 *  Run:        ed MYFILE.TXT
 * ======================================================================== */

#include "pod.h"
#include "pod_rt.h"       /* pod_svc: the services table, stashed by crt0 */
#include <string.h>
#include <stdlib.h>

POD_NAME("ed")
POD_VERSION("1.0")
POD_NEEDS(CAP_CONSOLE, "CONSOLE=draws the editor and reads the keyboard")
POD_NEEDS(CAP_FILES,   "FILES=loads and saves the file being edited")
POD_NEEDS(CAP_HEAP,    "HEAP=holds the text being edited")

/* ---------------------------------------------------------------- CONFIG */

/* COLS, ROWS and the word-wrap column are discovered at startup from the real
 * text grid (see layout()); the values here are only the fallback when the
 * screen size is unknown (e.g. no console). */
static int COLS    = 80;    /* text columns  */
static int ROWS    = 25;    /* text rows     */
static int wrapcol = 72;    /* word wrap column; 0 disables wrap */

#define MAXLEN      255     /* longest line, as in the Pascal original      */
#define TABSIZE     4
#define HIGHLIGHT   1       /* tint the line the cursor is on               */
#define USE_CP437   1       /* 0 if your font lacks the box-drawing glyphs  */

/* Logical colours, as COLOUR/GCOL use them. */
#define C_BLACK 0
#define C_RED   1
#define C_GREEN 2
#define C_YEL   3
#define C_BLUE  4
#define C_MAG   5
#define C_CYAN  6
#define C_WHITE 7

#if USE_CP437
#define B_TL 201
#define B_TR 187
#define B_BL 200
#define B_BR 188
#define B_H  205
#define B_V  186
#else
#define B_TL '+'
#define B_TR '+'
#define B_BL '+'
#define B_BR '+'
#define B_H  '-'
#define B_V  '|'
#endif

/* --------------------------------------------------------------- KEYCODES
 *
 *  The codes BerryBasiC's getkey service returns (see seed/seed.h SEED_KEY_*
 *  and drivers/usb_hid.h KEY_*).  Printable keys arrive as their character;
 *  the editing keys below arrive as these values.  This block is the only
 *  place they live.
 */
#define K_BS      8         /* Backspace */
#define K_TAB     9
#define K_ENTER   10        /* key() folds a serial CR (13) to this */
#define K_CTRLY   25        /* Ctrl-Y: delete line */
#define K_ESC     0x1B
#define K_LEFT    0x11
#define K_RIGHT   0x12
#define K_UP      0x13
#define K_DOWN    0x14
#define K_HOME    0x15
#define K_END     0x16
#define K_INS     0x17
#define K_DEL     0x7F
#define K_F1      0x101     /* help   */
#define K_F2      0x102     /* save   */
#define K_F3      0x103     /* open   */
#define K_F10     0x10A     /* menu   */
#define K_PGUP    0x10D
#define K_PGDN    0x10E

/* ------------------------------------------------------- PLATFORM LAYER
 *
 *  Everything hardware-facing is confined to these few functions.  They
 *  speak VDU control codes, which is the text-mode API the console driver
 *  already understands - the portable equivalent of the original's writes
 *  straight into segment 0xB800.
 */

static const BerryServices *S;

/* All screen output goes through the VDU stream: control codes (12 clear,
 * 17 colour, 23 cursor shape, 31 cursor position) are interpreted, and bytes
 * >= 32 print as glyphs. Plain putc would drop the control codes. */
static void out(int c)              { S->vdu(c); }
static void cls(void)               { out(12); }
static void at(int x, int y)        { out(31); out(x); out(y); }
static void fg(int c)               { out(17); out(c); }
static void bg(int c)               { out(17); out(128 + c); }
/* getkey returns LF (10) for Enter on a USB keyboard but CR (13) over a serial
 * line; fold both to K_ENTER so every input loop needs only one test. */
static int  key(void)               { int c = S->getkey(); return c == 13 ? K_ENTER : c; }

static void cursor(int on)
{
    int i;
    out(23); out(1); out(on ? 1 : 0);
    for (i = 0; i < 7; i++) out(0);
}

static void puts_at(int x, int y, const char *s)
{
    at(x, y);
    while (*s) out((unsigned char)*s++);
}

static void repeat_ch(int x, int y, int ch, int n)
{
    int i;
    at(x, y);
    for (i = 0; i < n; i++) out(ch);
}

/* --------------------------------------------------------------- LINES */

typedef struct Line {
    struct Line *prev, *next;
    int  len;
    char s[MAXLEN + 1];
} Line;

static Line *first;         /* head of the list                   */
static Line *top;           /* first line shown in the window     */
static Line *cur;           /* line the cursor is on              */

static int curx;            /* column within the line, 0-based    */
static int cury;            /* screen row of the cursor           */
static int coloff;          /* horizontal scroll offset           */
static int lineno = 1;      /* 1-based line number, for the status*/

static int win_x1, win_y1, win_x2, win_y2;   /* set by layout() from COLS/ROWS */

static int insert_mode = 1;
static int changed;
static char filename[64] = "NONAME.TXT";

static Line *line_alloc(void)
{
    Line *p = (Line *)malloc(sizeof(Line));
    if (!p) return 0;
    p->prev = p->next = 0;
    p->len = 0;
    p->s[0] = 0;
    return p;
}

static void free_all(void)
{
    Line *p = first, *q;
    while (p) { q = p; p = p->next; free(q); }
    first = top = cur = 0;
}

/* Insert a new line after `at_line`, carrying `text` (may be NULL). */
static Line *line_insert_after(Line *at_line, const char *text)
{
    Line *p = line_alloc();
    if (!p) return 0;
    if (text) {
        int n = (int)strlen(text);
        if (n > MAXLEN) n = MAXLEN;
        memcpy(p->s, text, n);
        p->s[n] = 0;
        p->len = n;
    }
    p->prev = at_line;
    p->next = at_line->next;
    if (p->next) p->next->prev = p;
    at_line->next = p;
    return p;
}

/* Unlink and free `p`, keeping first/top/cur valid.  Never empties the list. */
static void line_remove(Line *p)
{
    if (!p->prev && !p->next) {         /* the only line: just clear it */
        p->len = 0; p->s[0] = 0; curx = 0;
        return;
    }
    if (p->prev) p->prev->next = p->next;
    if (p->next) p->next->prev = p->prev;
    if (first == p) first = p->next;
    if (top   == p) top   = p->next ? p->next : p->prev;
    if (cur   == p) cur   = p->next ? p->next : p->prev;
    free(p);
}

/* ---------------------------------------------------------- FILE ACCESS */

static int load_file(const char *name)
{
    char buf[512];
    int h, n, i;
    Line *ln;

    free_all();
    first = line_alloc();
    if (!first) return -1;
    top = cur = first;
    curx = coloff = 0;
    lineno = 1;

    h = S->file_open(name, BERRY_FOPEN_READ);
    if (h <= 0) return 1;               /* no such file: start empty */

    ln = first;
    while ((n = S->file_read(h, buf, (int)sizeof buf)) > 0) {
        for (i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                Line *nl = line_insert_after(ln, 0);
                if (!nl) { S->file_close(h); return -1; }
                ln = nl;
            } else if (ln->len < MAXLEN) {
                ln->s[ln->len++] = c;
                ln->s[ln->len] = 0;
            }
        }
    }
    S->file_close(h);
    changed = 0;
    return 0;
}

static int save_file(const char *name)
{
    Line *p;
    int h = S->file_open(name, BERRY_FOPEN_WRITE);
    if (h <= 0) return -1;
    for (p = first; p; p = p->next) {
        if (p->len) S->file_write(h, p->s, p->len);
        if (p->next) S->file_write(h, "\n", 1);
    }
    S->file_close(h);
    changed = 0;
    return 0;
}

/* ------------------------------------------------------------- DRAWING */

static void draw_frame(void)
{
    int i, x, w;

    bg(C_BLUE); fg(C_WHITE);
    at(win_x1 - 1, win_y1 - 1); out(B_TL);
    repeat_ch(win_x1, win_y1 - 1, B_H, win_x2 - win_x1 + 1);
    at(win_x2 + 1, win_y1 - 1); out(B_TR);

    for (i = win_y1; i <= win_y2; i++) {
        at(win_x1 - 1, i); out(B_V);
        at(win_x2 + 1, i); out(B_V);
    }

    at(win_x1 - 1, win_y2 + 1); out(B_BL);
    repeat_ch(win_x1, win_y2 + 1, B_H, win_x2 - win_x1 + 1);
    at(win_x2 + 1, win_y2 + 1); out(B_BR);

    /* filename, centred in the top border */
    w = (int)strlen(filename) + 2;
    x = win_x1 + (win_x2 - win_x1 + 1 - w) / 2;
    fg(C_YEL);
    at(x, win_y1 - 1); out(' ');
    puts_at(x + 1, win_y1 - 1, filename);
    out(' ');
    fg(C_WHITE);
}

/* The menu bar. Each menu has a title shown on the bar and a NULL-terminated
 * list of dropdown items (see run_menu / menu_action below). */
static const char *file_items[] = {
    " New         ", " Open...     ", " Save        ", " Save As...  ", " Quit        ", 0
};
static const char *edit_items[] = {
    " Delete Line ", " Go to Top   ", " Go to Bottom", 0
};
static const char *help_items[] = {
    " Keys...     ", " About       ", 0
};
static const struct menu_def {
    const char *title;              /* as drawn on the bar, e.g. " File " */
    const char *const *items;
    int x;                          /* column of the title on the bar */
} menus[] = {
    { " File ", file_items, 2 },
    { " Edit ", edit_items, 9 },
    { " Help ", help_items, 16 },
};
#define NMENU ((int)(sizeof menus / sizeof menus[0]))

static int menu_count(const char *const *it) { int n = 0; while (it[n]) n++; return n; }

/* Draw the bar; `active` (0..NMENU-1) is highlighted, or -1 for none. */
static void draw_bar(int active)
{
    int i;
    bg(C_WHITE); fg(C_BLACK);
    repeat_ch(0, 0, ' ', COLS);
    for (i = 0; i < NMENU; i++) {
        if (i == active) bg(C_CYAN); else bg(C_WHITE);
        fg(C_BLACK);
        puts_at(menus[i].x, 0, menus[i].title);
    }
    bg(C_WHITE); fg(C_BLACK);
    if (COLS >= 26) puts_at(COLS - 26, 0, " ed - BerryBasiC editor ");
}

static void draw_menubar(void) { draw_bar(-1); }

static void draw_status(void)
{
    char b[64];
    int n = 0, i;
    long v;
    char t[12];

    bg(C_WHITE); fg(C_BLACK);
    repeat_ch(0, ROWS - 1, ' ', COLS);

    /* "  12:34  INS  *  " built by hand - no printf in a freestanding pod */
    b[n++] = ' '; b[n++] = ' ';
    v = lineno; i = 0;
    do { t[i++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (i) b[n++] = t[--i];
    b[n++] = ':';
    v = curx + 1; i = 0;
    do { t[i++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (i) b[n++] = t[--i];
    b[n++] = ' '; b[n++] = ' ';
    b[n++] = insert_mode ? 'I' : 'O';
    b[n++] = insert_mode ? 'N' : 'V';
    b[n++] = insert_mode ? 'S' : 'R';
    b[n++] = ' '; b[n++] = ' ';
    if (changed) { b[n++] = '*'; b[n++] = ' '; }
    b[n] = 0;
    puts_at(0, ROWS - 1, b);

    puts_at(COLS - 46, ROWS - 1,
            " F1 Help  F2 Save  F3 Open  F10 Menu  Esc Quit ");
}

static void draw_text(void)
{
    Line *p = top;
    int row, col, i;

    for (row = win_y1; row <= win_y2; row++) {
        int hl = (HIGHLIGHT && p == cur);
        bg(hl ? C_CYAN : C_BLUE);
        fg(C_WHITE);
        at(win_x1, row);
        col = 0;
        if (p) {
            for (i = coloff; i < p->len && col <= win_x2 - win_x1; i++, col++)
                out((unsigned char)p->s[i]);
        }
        while (col <= win_x2 - win_x1) { out(' '); col++; }
        if (p) p = p->next;
    }
}

static void place_cursor(void)
{
    at(win_x1 + curx - coloff, cury);
}

static void redraw(void)
{
    cursor(0);
    draw_menubar();
    draw_frame();
    draw_text();
    draw_status();
    place_cursor();
    cursor(1);
}

/* -------------------------------------------------------- SMALL DIALOGS */

static void message(const char *msg)
{
    int w = (int)strlen(msg) + 4;
    int x = (COLS - w) / 2, y = ROWS / 2 - 1, i;

    cursor(0);
    bg(C_WHITE); fg(C_BLACK);
    for (i = 0; i < 3; i++) repeat_ch(x, y + i, ' ', w);
    puts_at(x + 2, y + 1, msg);
    puts_at(x + 2, y + 2, " [ press a key ] ");
    key();
    redraw();
}

/* Read a line of text into buf.  Returns 0 if cancelled with Esc. */
static int prompt(const char *label, char *buf, int max)
{
    int w = COLS / 2;
    int x = (COLS - w) / 2, y = ROWS / 2 - 1;
    int n = 0, c, i;

    cursor(0);
    bg(C_WHITE); fg(C_BLACK);
    for (i = 0; i < 4; i++) repeat_ch(x, y + i, ' ', w);
    puts_at(x + 2, y + 1, label);
    buf[0] = 0;

    cursor(1);
    for (;;) {
        repeat_ch(x + 2, y + 2, ' ', w - 4);
        puts_at(x + 2, y + 2, buf);
        at(x + 2 + n, y + 2);
        c = key();
        if (c == K_ESC)   { redraw(); return 0; }
        if (c == K_ENTER) { redraw(); return n > 0; }
        if (c == K_BS)    { if (n) buf[--n] = 0; continue; }
        if (c >= 32 && c < 127 && n < max - 1) {
            buf[n++] = (char)c;
            buf[n] = 0;
        }
    }
}

/* A drop-down list. Returns the chosen index, or one of the NAV_* sentinels
 * (Esc / Left / Right) so the caller can move along the menu bar. Does not
 * redraw; the caller does that once the menu is dismissed. */
#define NAV_CANCEL (-1)
#define NAV_LEFT   (-2)
#define NAV_RIGHT  (-3)

static int popup(int x, int y, const char *const *items, int n)
{
    int sel = 0, i, c, w = 0;

    for (i = 0; i < n; i++) {
        int l = (int)strlen(items[i]);
        if (l > w) w = l;
    }
    w += 2;

    cursor(0);
    for (;;) {
        for (i = 0; i < n; i++) {
            bg(i == sel ? C_CYAN : C_WHITE);
            fg(C_BLACK);
            repeat_ch(x, y + i, ' ', w);
            puts_at(x + 1, y + i, items[i]);
        }
        c = key();
        if (c == K_UP)    { sel = sel ? sel - 1 : n - 1; continue; }
        if (c == K_DOWN)  { sel = (sel + 1) % n;         continue; }
        if (c == K_ENTER) return sel;
        if (c == K_ESC)   return NAV_CANCEL;
        if (c == K_LEFT)  return NAV_LEFT;    /* move to the menu on the left  */
        if (c == K_RIGHT) return NAV_RIGHT;   /* move to the menu on the right */
    }
}

/* ---------------------------------------------------------- NAVIGATION */

static void scroll_into_view(void)
{
    Line *p;
    int r;

    /* horizontal */
    if (curx < coloff) coloff = curx;
    if (curx - coloff > win_x2 - win_x1) coloff = curx - (win_x2 - win_x1);
    if (coloff < 0) coloff = 0;

    /* is cur visible?  walk down from top */
    p = top; r = win_y1;
    while (p && r <= win_y2) {
        if (p == cur) { cury = r; return; }
        p = p->next; r++;
    }
    /* not visible: is it above or below? */
    for (p = top; p; p = p->prev)
        if (p == cur) { top = cur; cury = win_y1; return; }

    /* below: scroll so cur sits on the last row */
    top = cur;
    for (r = win_y1; r < win_y2 && top->prev; r++) top = top->prev;
    cury = win_y2;
}

static void go_up(void)
{
    if (!cur->prev) return;
    cur = cur->prev; lineno--;
    if (curx > cur->len) curx = cur->len;
}

static void go_down(void)
{
    if (!cur->next) return;
    cur = cur->next; lineno++;
    if (curx > cur->len) curx = cur->len;
}

/* ------------------------------------------------------------- EDITING */

static void mark_changed(void) { changed = 1; }

static void do_wrap(void)
{
    int i, cut;
    Line *nl;

    if (!wrapcol || cur->len <= wrapcol) return;

    cut = wrapcol;
    while (cut > 0 && cur->s[cut] != ' ') cut--;
    if (cut == 0) return;                       /* one long word: leave it */

    nl = line_insert_after(cur, cur->s + cut + 1);
    if (!nl) return;

    for (i = cut; i > 0 && cur->s[i - 1] == ' '; i--) ;
    cur->s[i] = 0;
    cur->len = i;

    if (curx > cut) { curx -= cut + 1; cur = nl; lineno++; }
}

static void insert_char(int c)
{
    int i;
    if (cur->len >= MAXLEN) { message("Line too long."); return; }

    while (curx > cur->len) cur->s[cur->len++] = ' ';

    if (insert_mode || curx >= cur->len) {
        for (i = cur->len; i > curx; i--) cur->s[i] = cur->s[i - 1];
        cur->len++;
    }
    cur->s[curx++] = (char)c;
    cur->s[cur->len] = 0;
    mark_changed();
    do_wrap();
}

static void do_backspace(void)
{
    int i, join;

    if (curx > 0) {
        for (i = curx - 1; i < cur->len; i++) cur->s[i] = cur->s[i + 1];
        cur->len--; curx--;
        mark_changed();
        return;
    }
    if (!cur->prev) return;

    /* join this line onto the end of the previous one */
    if (cur->prev->len + cur->len > MAXLEN) {
        message("Joined line would be too long.");
        return;
    }
    join = cur->prev->len;
    memcpy(cur->prev->s + join, cur->s, cur->len);
    cur->prev->len += cur->len;
    cur->prev->s[cur->prev->len] = 0;
    { Line *dead = cur; cur = cur->prev; lineno--; line_remove(dead); }
    curx = join;
    mark_changed();
}

static void do_delete(void)
{
    int i;

    if (curx < cur->len) {
        for (i = curx; i < cur->len; i++) cur->s[i] = cur->s[i + 1];
        cur->len--;
        mark_changed();
        return;
    }
    if (!cur->next) return;

    if (cur->len + cur->next->len > MAXLEN) {
        message("Joined line would be too long.");
        return;
    }
    memcpy(cur->s + cur->len, cur->next->s, cur->next->len);
    cur->len += cur->next->len;
    cur->s[cur->len] = 0;
    line_remove(cur->next);
    mark_changed();
}

static void do_enter(void)
{
    Line *nl;

    while (curx > cur->len) cur->s[cur->len++] = ' ';
    cur->s[cur->len] = 0;

    nl = line_insert_after(cur, cur->s + curx);
    if (!nl) { message("Out of memory."); return; }

    cur->s[curx] = 0;
    cur->len = curx;
    cur = nl;
    lineno++;
    curx = 0;
    coloff = 0;
    mark_changed();
}

static void do_tab(void)
{
    int i, n = TABSIZE - (curx % TABSIZE);
    for (i = 0; i < n; i++) insert_char(' ');
}

/* --------------------------------------------------------- MENU ACTIONS */

static void help_screen(void);          /* defined below; used by the Help menu */

static void do_new(void)
{
    free_all();
    first = line_alloc();
    top = cur = first;
    curx = coloff = 0;
    lineno = 1;
    changed = 0;
    strcpy(filename, "NONAME.TXT");
}

/* Delete the current line (also Ctrl-Y). Never empties the list. */
static void delete_line(void)
{
    if (cur->next || cur->prev) {
        Line *dead = cur;
        if (cur->next) cur = cur->next; else { cur = cur->prev; lineno--; }
        line_remove(dead);
    } else { cur->len = 0; cur->s[0] = 0; }
    curx = 0;
    mark_changed();
}

static void goto_top(void)    { cur = first; curx = coloff = 0; lineno = 1; scroll_into_view(); }
static void goto_bottom(void) { while (cur->next) { cur = cur->next; lineno++; } curx = 0; scroll_into_view(); }

/* Run the chosen (menu, item). Returns 1 if the editor should quit. */
static int menu_action(int menu, int item)
{
    char buf[64];

    if (menu == 0) {                                  /* File */
        switch (item) {
        case 0: do_new(); break;
        case 1: if (prompt("Open which file?", buf, sizeof buf)) {
                    if (load_file(buf) < 0) message("Could not open that file.");
                    else strcpy(filename, buf);
                } break;
        case 2: if (save_file(filename) < 0) message("Could not save.");
                else message("Saved."); break;
        case 3: if (prompt("Save as?", buf, sizeof buf)) {
                    if (save_file(buf) < 0) message("Could not save.");
                    else { strcpy(filename, buf); message("Saved."); }
                } break;
        case 4: return 1;                             /* Quit */
        }
    } else if (menu == 1) {                           /* Edit */
        switch (item) {
        case 0: delete_line();  break;
        case 1: goto_top();     break;
        case 2: goto_bottom();  break;
        }
    } else {                                          /* Help */
        switch (item) {
        case 0: help_screen(); break;
        case 1: message("ed - a full-screen text editor for BerryBasiC"); break;
        }
    }
    return 0;
}

/* F10: drive the menu bar. Left/Right move between File/Edit/Help, Down/Enter
 * pick an item, Esc leaves. Returns 1 if the editor should quit. */
static int run_menu(void)
{
    int active = 0, quit = 0;

    for (;;) {
        draw_bar(active);
        int sel = popup(menus[active].x, 1, menus[active].items,
                        menu_count(menus[active].items));
        if (sel == NAV_LEFT)  { active = (active + NMENU - 1) % NMENU; continue; }
        if (sel == NAV_RIGHT) { active = (active + 1) % NMENU;         continue; }
        if (sel == NAV_CANCEL) break;                 /* Esc: leave the menu */
        quit = menu_action(active, sel);
        break;
    }
    redraw();
    return quit;
}

static void help_screen(void)
{
    static const char *lines[] = {
        "ed - keys",
        "",
        "  Arrows / Home / End      move",
        "  PgUp / PgDn              page",
        "  Enter                    split line",
        "  Backspace / Del          join or delete",
        "  Tab                      indent",
        "  Ctrl-Y                   delete line",
        "  Ins                      insert / overwrite",
        "  F2 / F3                  save / open",
        "  F1 / F10                 help / menu",
        "  Esc                      quit",
        0
    };
    int i;

    cursor(0);
    bg(C_BLACK); fg(C_WHITE);
    cls();
    for (i = 0; lines[i]; i++) puts_at(4, 2 + i, lines[i]);
    key();
    redraw();
}

/* ---------------------------------------------------------------- MAIN */

/* Size the editor to the real text grid. Falls back to the 80x25 defaults when
 * the console cannot report a size (e.g. the host build reports 0). */
static void layout(void)
{
    int cols = 0, rows = 0;
    S->screen_size(&cols, &rows);
    if (cols >= 20 && cols <= 400) COLS = cols;
    if (rows >= 8  && rows <= 200) ROWS = rows;

    win_x1 = 2;
    win_y1 = 2;
    win_x2 = COLS - 3;                  /* border + a right margin */
    win_y2 = ROWS - 4;                  /* leave the status bar (ROWS-1) + border */

    wrapcol = win_x2 - win_x1;          /* wrap at the window's right edge */
    if (wrapcol > MAXLEN - 1) wrapcol = MAXLEN - 1;
    if (wrapcol < 8)          wrapcol = 0;   /* too narrow: don't wrap */
}

int main(int argc, char **argv)
{
    int c, i, quit = 0;

    S = pod_svc;                        /* crt0 stashed the services table here */
    layout();                           /* adapt to the real screen size */

    if (argc > 1 && argv[1] && argv[1][0]) {
        int n = (int)strlen(argv[1]);
        if (n > (int)sizeof(filename) - 1) n = (int)sizeof(filename) - 1;
        memcpy(filename, argv[1], n);
        filename[n] = 0;
    }

    if (load_file(filename) < 0) {
        S->puts("ed: out of memory\n", 18);
        return 1;
    }

    bg(C_BLUE); fg(C_WHITE);
    cls();
    redraw();

    while (!quit) {
        c = key();

        switch (c) {
        case K_LEFT:
            if (curx > 0) curx--;
            else if (cur->prev) { go_up(); curx = cur->len; }
            break;
        case K_RIGHT:
            if (curx < cur->len) curx++;
            else if (cur->next) { go_down(); curx = 0; }
            break;
        case K_UP:    go_up();   break;
        case K_DOWN:  go_down(); break;
        case K_HOME:  curx = 0; coloff = 0; break;
        case K_END:   curx = cur->len; break;

        case K_PGUP:
            for (i = 0; i < win_y2 - win_y1 && cur->prev; i++) go_up();
            break;
        case K_PGDN:
            for (i = 0; i < win_y2 - win_y1 && cur->next; i++) go_down();
            break;

        case K_ENTER: do_enter();     break;
        case K_BS:    do_backspace(); break;
        case K_DEL:   do_delete();    break;
        case K_TAB:   do_tab();       break;
        case K_INS:   insert_mode = !insert_mode; break;

        case K_CTRLY:
            delete_line();
            break;

        case K_F1:
            help_screen();
            break;

        case K_F2:
            if (save_file(filename) < 0) message("Could not save.");
            else message("Saved.");
            break;

        case K_F3: {
            char buf[64];
            if (prompt("Open which file?", buf, sizeof buf)) {
                if (load_file(buf) < 0) message("Could not open that file.");
                else strcpy(filename, buf);
            }
            break;
        }

        case K_F10:
            quit = run_menu();
            break;

        case K_ESC:
            quit = 1;
            break;

        default:
            if (c >= 32 && c < 256) insert_char(c);
            break;
        }

        if (quit && changed) {
            char buf[8];
            if (prompt("Save before leaving?  (y/n)", buf, sizeof buf)) {
                if (buf[0] == 'y' || buf[0] == 'Y') save_file(filename);
            }
        }

        scroll_into_view();
        redraw();
    }

    cursor(1);
    bg(C_BLACK); fg(C_WHITE);
    cls();
    free_all();
    return 0;
}

/* ===========================================================================
 *  Extending it
 *
 *  Block operations were the one substantial feature left out.  The original
 *  kept `selected`, `selstart` and `selend` on every line and shuttled the
 *  marked region through a temp.xyz file.  With a heap available the tidier
 *  route is a clipboard held as its own Line list: mark with Ctrl-B/Ctrl-K,
 *  copy by cloning the marked span, paste by splicing the clone in after the
 *  cursor.  draw_text() already knows how to tint a line - give it a second
 *  colour for "inside the block" and the display side is done.
 *
 *  Justification is likewise a short addition on top of do_wrap(): after the
 *  split, pad interior spaces until the line reaches wrapcol.
 * ======================================================================== */