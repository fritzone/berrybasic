/* ===========================================================================
 *  ed - a small full-screen text editor for BerryBasiC
 *
 *  Shipped to /sys as the `ed` command (EDIT is a reserved keyword - the
 *  built-in line editor - so this file editor uses the classic short name).
 *
 *  Run:        ed MYFILE.TXT
 * ======================================================================== */

#include "pod.h"
#include "pod_rt.h"       /* berry_svc: the services table, stashed by crt0 */
#include <string.h>
#include <stdlib.h>
#include <dirent.h>       /* opendir/readdir: browse the card in the dialogs */
#include <unistd.h>       /* getcwd */
#include <sys/stat.h>     /* mkdir */

POD_NAME("ed")
POD_VERSION("1.0")
POD_NEEDS(CAP_GRAPHICS, "GRAPHICS=draws the screen, double-buffered so it never flickers")
POD_NEEDS(CAP_CONSOLE,  "CONSOLE=reads the keyboard")
POD_NEEDS(CAP_FILES,    "FILES=loads and saves the file being edited")
POD_NEEDS(CAP_DIRS,     "DIRS=the Open/Save dialogs browse folders and make new ones")
POD_NEEDS(CAP_HEAP,     "HEAP=holds the text being edited")
POD_NEEDS(CAP_SPAWN,    "SPAWN=Run runs the current BASIC program")
POD_NEEDS(CAP_DEBUG,    "DEBUG=Debug steps through the program with breakpoints")
POD_NEEDS(CAP_VARS,     "VARS=the debugger shows variable values")

/* ---------------------------------------------------------------- CONFIG */

/* COLS, ROWS and the word-wrap column are discovered at startup from the real
 * text grid (see layout()); the values here are only the fallback when the
 * screen size is unknown (e.g. no console). */
static int COLS    = 80;    /* text columns  */
static int ROWS    = 25;    /* text rows     */
static int wrapcol = 72;    /* word wrap column; 0 disables wrap */

#define MAXLEN      255     /* longest line, as in the Pascal original      */
#define TABSIZE     4
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
#define K_F4      0x104     /* follow the line-number reference under the cursor */
#define K_F5      0x105     /* run the BASIC program */
#define K_F6      0x106     /* find next */
#define K_F7      0x107     /* new folder (in the file dialog) */
#define K_F8      0x108     /* jump back */
#define K_F9      0x109     /* PROC/FN outline */
#define K_F10     0x10A     /* menu   */
#define K_PGUP    0x10D
#define K_PGDN    0x10E

/* Modifier bits from the keymods service (see seed/seed.h SEED_KMOD_*). */
#define KMOD_SHIFT 0x001
#define KMOD_CTRL  0x002
#define KMOD_ALT   0x004

/* ------------------------------------------------------- PLATFORM LAYER
 *
 *  ed draws in GRAPHICS mode with DOUBLE BUFFERING, using the system's own
 *  console font (con_glyph / con_font). Every glyph is composed into the
 *  off-screen back buffer and the finished frame is shown in one go (key()
 *  flips just before it blocks for input), so a full redraw on every keypress
 *  never flickers - the old VDU text-mode path repainted the visible screen
 *  cell by cell, which did. The primitives below keep the rest of the editor
 *  thinking in character cells; only these functions know about pixels.
 */

static const BerryServices *S;

static int CELLW = 8, CELLH = 16;   /* console font cell in pixels (con_font) */

/* BBC logical colours 0..7 as 0xRRGGBB, matching the system text palette. */
static const unsigned pal[8] = {
    0x000000, 0xFF0000, 0x00FF00, 0xFFFF00,
    0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF
};
static unsigned cur_fg = 0xFFFFFF;  /* current foreground */
static unsigned cur_bg = 0x000000;  /* current background */
static int pen_x, pen_y;            /* pen position, in character cells */

static void fg(int c) { cur_fg = pal[c & 7]; }
static void bg(int c) { cur_bg = pal[c & 7]; }
static void fgrgb(unsigned c) { cur_fg = c; }   /* arbitrary 0xRRGGBB (syntax colours) */
static void bgrgb(unsigned c) { cur_bg = c; }
static void at(int x, int y) { pen_x = x; pen_y = y; }

/* Draw one glyph cell in the current colours, then advance the pen. */
static void out(int ch)
{
    S->con_glyph(pen_x * CELLW, pen_y * CELLH, ch & 0xFF, cur_fg, cur_bg);
    pen_x++;
}
static void cls(void) { S->gfx_clear(cur_bg); }

/* Present the composed frame, then block for a key (folding a serial CR to LF). */
static int key(void) { S->gfx_flip(); int c = S->getkey(); return c == 13 ? K_ENTER : c; }

/* The text caret: an underline in the foreground colour at cell (col,row),
 * drawn last so it sits on top of the frame. */
static void caret(int col, int row)
{
    int px = col * CELLW, py = row * CELLH + CELLH - 2;
    S->gfx_fillrect(px, py, px + CELLW - 1, py + 1, cur_fg);
}
static void cursor(int on) { (void)on; }   /* the caret is part of the frame now */

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

static Line *sel_anchor = 0;   /* selection anchor line; 0 = no selection */
static int   sel_acol   = 0;   /* selection anchor column                 */

static int win_x1, win_y1, win_x2, win_y2;   /* set by layout() from COLS/ROWS */
static int gutter_w = 5;      /* line-number gutter width in cells (recomputed)   */
static int is_basic = 0;      /* current file looks like a BASIC program          */
static int auto_num = 1;      /* BASIC auto line-numbering: gutter + managed nums  */

/* Debugger breakpoints the editor owns, as BASIC line numbers. They show as a
 * dot in the gutter and are handed to the interpreter's debugger when Debug
 * runs the program. Pure editor state until then (see run_debug). */
#define MAX_BREAKS 64
static int brk_lines[MAX_BREAKS];
static int brk_n = 0;
static int has_brk(int ln) { for (int i = 0; i < brk_n; i++) if (brk_lines[i] == ln) return 1; return 0; }

/* BASIC line-number / body split (defined with the drawing code below, but used
 * earlier by the status bar). See body_off_of() for what it means. */
static int body_off(void);
static int line_num(Line *p);        /* the BASIC line number of p, or -1 */

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
    " Undo         Ctrl-Z ",
    " Redo      Sh-Ctrl-Z ",
    "-",                            /* separator */
    " Cut          Ctrl-X ",
    " Copy         Ctrl-C ",
    " Paste        Ctrl-V ",
    "-",                            /* separator */
    " Delete Line  Ctrl-Y ",
    " Go to Top           ",
    " Go to Bottom        ",
    "-",                            /* separator */
    " Find...      Ctrl-F ",
    " Find Next    F6     ",
    " Replace...   Ctrl-R ",
    0
};
static const char *help_items[] = {
    " About       ", 0
};
static const char *basic_items[] = {    /* BASIC-only menu (see menu_visible()) */
    " Run             F5     ",     /* 0 */
    " Debug           Ctrl-D ",     /* 1 */
    " Toggle Break    Ctrl-B ",     /* 2 */
    " Clear Breaks           ",     /* 3 */
    "-",                            /* 4 separator */
    " Auto Number     Alt-N  ",     /* 5 */
    "-",                            /* 6 separator */
    " Procedures...   F9     ",     /* 7 */
    " Go to Line...   Ctrl-G ",     /* 8 */
    " Follow Ref      F4     ",     /* 9 */
    " Jump Back       F8     ",     /* 10 */
    " Check Refs             ",     /* 11 */
    0
};
static const struct menu_def {
    const char *title;              /* as drawn on the bar, e.g. " File " */
    const char *const *items;
} menus[] = {
    { " File ",  file_items  },
    { " Edit ",  edit_items  },
    { " Basic ", basic_items },       /* index BASIC_MENU: only for BASIC files */
    { " Help ",  help_items  },       /* Help stays last, as users expect */
};
#define NMENU_ALL ((int)(sizeof menus / sizeof menus[0]))
#define BASIC_MENU 2

/* The Basic menu (Run + Auto Number) is only offered while editing a BASIC file. */
static int menu_visible(int i) { return i != BASIC_MENU || is_basic; }

/* Column where menu i's title is drawn: titles laid left to right from column 2,
 * skipping any hidden menu so the bar shows no gap. */
static int menu_col(int i)
{
    int x = 2, j;
    for (j = 0; j < i; j++) if (menu_visible(j)) x += (int)strlen(menus[j].title);
    return x;
}

/* The next visible menu from i in direction dir (+1/-1), wrapping. */
static int menu_step(int i, int dir)
{
    do { i = (i + dir + NMENU_ALL) % NMENU_ALL; } while (!menu_visible(i));
    return i;
}

static int menu_count(const char *const *it) { int n = 0; while (it[n]) n++; return n; }

/* Draw the bar; `active` (0..NMENU-1) is highlighted, or -1 for none. */
static void draw_bar(int active)
{
    int i;
    bg(C_WHITE); fg(C_BLACK);
    repeat_ch(0, 0, ' ', COLS);
    for (i = 0; i < NMENU_ALL; i++) {
        if (!menu_visible(i)) continue;
        if (i == active) bg(C_CYAN); else bg(C_WHITE);
        fg(C_BLACK);
        puts_at(menu_col(i), 0, menus[i].title);
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
    v = (curx - body_off()) + 1; i = 0;      /* column within the code body (matches the caret) */
    do { t[i++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (i) b[n++] = t[--i];
    b[n++] = ' '; b[n++] = ' ';
    b[n++] = insert_mode ? 'I' : 'O';
    b[n++] = insert_mode ? 'N' : 'V';
    b[n++] = insert_mode ? 'S' : 'R';
    b[n++] = ' '; b[n++] = ' ';
    if (is_basic) {                                  /* BASIC line-number mode */
        const char *t2 = auto_num ? "AUTO-NUM" : "MANUAL";
        while (*t2) b[n++] = *t2++;
        b[n++] = ' '; b[n++] = ' ';
    }
    if (changed) { b[n++] = '*'; b[n++] = ' '; }
    b[n] = 0;
    puts_at(0, ROWS - 1, b);

    if (is_basic)
        puts_at(COLS - 55, ROWS - 1,
                " F1 Help  F2 Save  F3 Open  F5 Run  F10 Menu  Esc Quit ");
    else
        puts_at(COLS - 46, ROWS - 1,
                " F1 Help  F2 Save  F3 Open  F10 Menu  Esc Quit ");
}

/* ----------------------------------------------------------- SELECTION */

static void mark_changed(void);        /* defined in the editing section below */
static void undo_record(void);         /* snapshot before a discrete edit (undo module) */
static void undo_record_typing(void);  /* snapshot before a run of typed characters      */
static void nav_reset(void);           /* clear the reference-jump back stack             */

static int line_index(Line *t)
{
    int i = 0;
    for (Line *p = first; p; p = p->next, i++) if (p == t) return i;
    return -1;
}

/* The line at index idx (clamped to the list), or first if idx is out of range.
 * Used to restore cursor/scroll by index after the line list is rebuilt. */
static Line *line_at(int idx)
{
    Line *p = first;
    int i = 0;
    while (p && p->next && i < idx) { p = p->next; i++; }
    return p ? p : first;
}

/* Normalise the selection into start (sl,sc) <= end (el,ec). 1 if non-empty. */
static int sel_ordered(Line **sl, int *sc, Line **el, int *ec)
{
    int ai, ci;
    if (!sel_anchor) return 0;
    ai = line_index(sel_anchor); ci = line_index(cur);
    if (ai < ci || (ai == ci && sel_acol <= curx)) { *sl = sel_anchor; *sc = sel_acol; *el = cur; *ec = curx; }
    else                                           { *sl = cur; *sc = curx; *el = sel_anchor; *ec = sel_acol; }
    return !(*sl == *el && *sc == *ec);
}

/* Copy the selection to the system clipboard (lines joined with '\n'). */
static void sel_copy(void)
{
    Line *sl, *el, *p; int sc, ec, i;
    long n, k;
    char *buf;
    if (!sel_ordered(&sl, &sc, &el, &ec)) return;
    if (sl == el) n = ec - sc;
    else { n = (sl->len - sc) + 1; for (p = sl->next; p && p != el; p = p->next) n += p->len + 1; n += ec; }
    buf = (char *)malloc(n + 1);
    if (!buf) return;
    k = 0;
    if (sl == el) { for (i = sc; i < ec; i++) buf[k++] = sl->s[i]; }
    else {
        for (i = sc; i < sl->len; i++) buf[k++] = sl->s[i]; buf[k++] = '\n';
        for (p = sl->next; p && p != el; p = p->next) { for (i = 0; i < p->len; i++) buf[k++] = p->s[i]; buf[k++] = '\n'; }
        for (i = 0; i < ec; i++) buf[k++] = el->s[i];
    }
    buf[k] = 0;
    S->clip_set(buf, (int)k);
    free(buf);
}

/* Delete the selected text, leaving the cursor at its start; clears selection. */
static void sel_delete(void)
{
    Line *sl, *el; int sc, ec, i;
    if (!sel_ordered(&sl, &sc, &el, &ec)) { sel_anchor = 0; return; }
    if (sl == el) {
        for (i = ec; i <= sl->len; i++) sl->s[sc + (i - ec)] = sl->s[i];
        sl->len -= (ec - sc); sl->s[sl->len] = 0;
    } else {
        int tail = el->len - ec;
        Line *after = el->next, *p, *q;
        sl->s[sc] = 0; sl->len = sc;
        for (i = 0; i < tail && sl->len < MAXLEN; i++) sl->s[sl->len++] = el->s[ec + i];
        sl->s[sl->len] = 0;
        for (p = sl->next; p && p != after; p = q) { q = p->next; free(p); }   /* free middle + el */
        sl->next = after; if (after) after->prev = sl;
    }
    cur = sl; curx = sc; top = sl;         /* top may have pointed at a freed line */
    sel_anchor = 0;
    lineno = line_index(cur) + 1;
    mark_changed();
}

/* Paste helpers: insert one char / split the line, without word-wrap. */
static void paste_char(int c)
{
    int i;
    if (cur->len >= MAXLEN) return;
    for (i = cur->len; i > curx; i--) cur->s[i] = cur->s[i - 1];
    cur->s[curx++] = (char)c; cur->len++; cur->s[cur->len] = 0;
}
static void paste_newline(void)
{
    Line *nl = line_insert_after(cur, cur->s + curx);
    if (!nl) return;
    cur->s[curx] = 0; cur->len = curx;
    cur = nl; curx = 0; lineno++;
}

/* Paste the clipboard at the cursor (replacing any selection). */
static void do_paste(void)
{
    int n = S->clip_len(), i;
    char *buf;
    if (n <= 0) return;
    undo_record();
    if (sel_anchor) sel_delete();
    buf = (char *)malloc(n + 1);
    if (!buf) return;
    S->clip_get(buf, n);
    for (i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\r') ;                                  /* drop CR */
        else if (c == '\n') paste_newline();
        else if (c == '\t') paste_char(' ');
        else if ((unsigned char)c >= 32) paste_char(c);
    }
    free(buf);
    mark_changed();
}

/* -------------------------------------------------------------- DRAW */

/* ---------------------------------------------------- SYNTAX + GUTTER */

static int total_lines(void)
{
    int n = 0; Line *p = first;
    while (p) { n++; p = p->next; }
    return n;
}

/* In a BASIC file the meaningful line number is the *logical* one written at the
 * start of the source line, so we show that in the gutter and hide it from the
 * body (otherwise both the physical and the logical numbers are on screen, which
 * is what confused the eye). These two helpers describe that split:
 *   lead_digits(p) - how many leading characters of p are the decimal number.
 *   body_off_of(p) - where the editable body starts: past the number and the one
 *                    space after it. 0 for non-BASIC files or a numberless line,
 *                    so every editor primitive below reduces to its old behaviour
 *                    when body_off is 0. */
static int lead_digits(Line *p)
{
    int i = 0;
    if (!p) return 0;
    while (i < p->len && p->s[i] >= '0' && p->s[i] <= '9') i++;
    return i;
}
static int body_off_of(Line *p)
{
    int i;
    if (!is_basic || !auto_num || !p) return 0; /* only auto-numbering hides the number */
    i = lead_digits(p);
    if (i == 0) return 0;                       /* no leading number: nothing to hide */
    if (i < p->len && p->s[i] == ' ') i++;      /* swallow one separating space */
    return i;
}
static int body_off(void) { return body_off_of(cur); }

/* Size the gutter to the widest number it must hold + a separator cell: for a
 * BASIC file that is the widest *logical* number, otherwise the physical line
 * count. (>=3 digits keeps small files tidy.) Recomputed each redraw; cheap. */
static void update_gutter(void)
{
    int d = 1;
    if (is_basic && !auto_num) {                /* manual BASIC: no gutter, inline numbers */
        gutter_w = 0;
        return;
    }
    if (is_basic) {                             /* auto BASIC: widest logical number */
        Line *p;
        for (p = first; p; p = p->next) { int n = lead_digits(p); if (n > d) d = n; }
    } else {                                    /* plain text: physical line count */
        int t = total_lines();
        while (t >= 10) { t /= 10; d++; }
    }
    if (d < 3) d = 3;
    /* digits + one trailing space, and for BASIC a leading column reserved for
     * the breakpoint marker so the '*' never overwrites the first digit. */
    gutter_w = d + 1 + (is_basic ? 1 : 0);
}

/* Does the filename end in ".BAS" (any case)?  Then it is syntax-coloured. */
static int name_is_basic(const char *fn)
{
    int n = 0; while (fn[n]) n++;
    if (n < 4) return 0;
    { const char *e = fn + n - 4;
      char a = e[1], b = e[2], c = e[3];
      if (a >= 'a') a -= 32; if (b >= 'a') b -= 32; if (c >= 'a') c -= 32;
      return e[0] == '.' && a == 'B' && b == 'A' && c == 'S'; }
}

/* v (>=0) as decimal into buf (no NUL); returns the digit count. */
static int fmt_uint(char *buf, int v)
{
    char t[12]; int n = 0, i;
    if (v <= 0) { buf[0] = '0'; return 1; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    for (i = 0; i < n; i++) buf[i] = t[n - 1 - i];
    return n;
}

/* --- a light BASIC highlighter: one colour class per character ---------- */
enum { HL_NORM = 0, HL_KEYWORD, HL_STRING, HL_COMMENT, HL_NUMBER, HL_LINENO };

static const char *const kw[] = {
    "PRINT","INPUT","IF","THEN","ELSE","ELSEIF","ENDIF","FOR","TO","STEP","NEXT",
    "WHILE","ENDWHILE","WEND","REPEAT","UNTIL","DO","LOOP","GOTO","GOSUB","RETURN",
    "END","STOP","LET","DIM","DATA","READ","RESTORE","DEF","PROC","FN","ENDPROC",
    "LOCAL","CALL","CLS","CLG","MODE","GCOL","COLOUR","COLOR","PLOT","DRAW","MOVE",
    "LINE","RECTANGLE","CIRCLE","VDU","RUN","LIST","NEW","LOAD","SAVE","CHAIN",
    "CLEAR","ON","OFF","AND","OR","NOT","EOR","MOD","DIV","TRUE","FALSE","TAB",
    "SPC","AT","BY","EXIT","CONTINUE","CASE","WHEN","OTHERWISE","ENDCASE","TRY",
    "CATCH","ENDTRY","RAISE","TYPE","ENDTYPE","IMPORT","EXEC","EVAL","SOUND","TONE",
    "WAIT","OPENIN","OPENOUT","OPENUP","CLOSE","BPUT","BGET","MKDIR","RMDIR",
    "DELETE","RENAME","CD", 0
};
static int upc(int c)     { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int is_alpha(int c){ return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static int is_digit(int c){ return c >= '0' && c <= '9'; }
static int is_kw(const char *w, int len)
{
    int k, j;
    for (k = 0; kw[k]; k++) {
        for (j = 0; j < len && kw[k][j] && upc(w[j]) == kw[k][j]; j++) ;
        if (j == len && kw[k][j] == 0) return 1;
    }
    return 0;
}
static void hl_basic(const char *s, int len, unsigned char *cls)
{
    int i = 0;
    while (i < len && is_digit(s[i])) cls[i++] = HL_LINENO;    /* leading line number */
    while (i < len) {
        int c = (unsigned char)s[i];
        if (c == '"') {                                        /* string literal */
            cls[i++] = HL_STRING;
            while (i < len && s[i] != '"') cls[i++] = HL_STRING;
            if (i < len) cls[i++] = HL_STRING;
        } else if (c == '\'') {                                /* ' comment to EOL */
            while (i < len) cls[i++] = HL_COMMENT;
        } else if (is_alpha(c)) {                              /* word: keyword or name */
            int j = i;
            while (j < len && (is_alpha(s[j]) || is_digit(s[j]))) j++;
            if (j - i == 3 && upc(s[i]) == 'R' && upc(s[i+1]) == 'E' && upc(s[i+2]) == 'M') {
                while (i < len) cls[i++] = HL_COMMENT;         /* REM comment to EOL */
            } else {
                unsigned char klass = is_kw(s + i, j - i) ? HL_KEYWORD : HL_NORM;
                while (i < j) cls[i++] = klass;
            }
        } else if (is_digit(c)) {                              /* number */
            while (i < len && (is_digit(s[i]) || s[i] == '.')) cls[i++] = HL_NUMBER;
        } else {
            cls[i++] = HL_NORM;
        }
    }
}
static unsigned syn_color(int cls)
{
    switch (cls) {
        case HL_KEYWORD: return 0xFFE060;   /* warm yellow */
        case HL_STRING:  return 0x80FF80;   /* green       */
        case HL_COMMENT: return 0x9FB0D0;   /* muted grey  */
        case HL_NUMBER:  return 0xFF9C60;   /* orange      */
        case HL_LINENO:  return 0x50E0FF;   /* cyan        */
        default:         return 0xFFFFFF;   /* white       */
    }
}

static void draw_text(void)
{
    Line *p = top;
    int row, col, i;
    Line *sl = 0, *el = 0; int sc = 0, ec = 0;
    int has_sel, sl_idx = -1, el_idx = -1, top_idx;
    int gw = gutter_w, tx1 = win_x1 + gw, tw = win_x2 - tx1;
    unsigned char clsbuf[MAXLEN + 1];

    has_sel = sel_ordered(&sl, &sc, &el, &ec);
    top_idx = line_index(top);
    if (has_sel) { sl_idx = line_index(sl); el_idx = line_index(el); }

    for (row = win_y1; row <= win_y2; row++) {
        int lidx = top_idx + (row - win_y1);
        int pfx  = body_off_of(p);            /* leading number shown in the gutter, not the body */
        int hl_lo = -1, hl_hi = -1, hl_on = 0;
        if (has_sel && p && lidx >= sl_idx && lidx <= el_idx) {
            hl_lo = (lidx == sl_idx) ? sc : 0;
            hl_hi = (lidx == el_idx) ? ec : p->len + 1;   /* +1 shows the newline is in the range */
        }

        /* gutter: right-aligned line number (blank past end of file). For an
         * auto-numbered BASIC file that is the logical number taken from the
         * source line, coloured like the code's own line-number token; otherwise
         * the physical row number. Manual BASIC has gw==0 (no gutter at all). */
        if (gw > 0) {
            bgrgb(0x0000A0);                 /* a darker shade of the window blue, not black */
            at(win_x1, row);
            if (p && is_basic && auto_num) {
                int nd = lead_digits(p), k, pad = gw - 1 - nd;
                fgrgb(syn_color(HL_LINENO));
                for (i = 0; i < pad; i++) out(' ');
                for (k = 0; k < nd; k++) out((unsigned char)p->s[k]);
                out(' ');
            } else if (p) {
                char num[12]; int nl = fmt_uint(num, lidx + 1), pad = gw - 1 - nl;
                fgrgb(0x8090C0);
                for (i = 0; i < pad; i++) out(' ');
                for (i = 0; i < nl; i++) out((unsigned char)num[i]);
                out(' ');
            } else {
                fgrgb(0x8090C0);
                for (i = 0; i < gw; i++) out(' ');
            }
        }

        /* a breakpoint dot in the leftmost gutter cell (over the number padding) */
        if (gw > 0 && p && is_basic && has_brk(line_num(p))) {
            bgrgb(0x0000A0); fgrgb(0xFF4040); at(win_x1, row); out('*');
        }

        if (p && is_basic) { hl_basic(p->s, p->len, clsbuf); hl_on = 1; }

        /* body: screen column `col` shows buffer index pfx + coloff + col, so the
         * hidden number scrolls off to the gutter and the code sits at the left. */
        at(tx1, row);
        for (col = 0; col <= tw; col++) {
            i = pfx + coloff + col;
            int selc = (i >= hl_lo && i < hl_hi);
            if (selc) { bgrgb(pal[C_CYAN]); fgrgb(pal[C_BLACK]); }
            else {
                bgrgb(pal[C_BLUE]);
                if (hl_on && i < p->len) fgrgb(syn_color(clsbuf[i]));
                else                     fgrgb(pal[C_WHITE]);
            }
            if (p && i < p->len) out((unsigned char)p->s[i]);
            else                 out(' ');
        }
        if (p) p = p->next;
    }
}

static void place_cursor(void)
{
    fg(C_WHITE);                        /* a white caret on the blue text area */
    caret(win_x1 + gutter_w + (curx - body_off()) - coloff, cury);
}

static void redraw(void)
{
    cursor(0);
    is_basic = name_is_basic(filename);    /* colour + logical-number gutter for .BAS   */
    update_gutter();                       /* size the gutter (needs is_basic set first) */
    draw_menubar();
    draw_frame();
    draw_text();
    draw_status();
    place_cursor();
    cursor(1);
}

/* -------------------------------------------------------- DIALOGS
 *
 *  A proper modal dialog: a bordered, titled box with a message and a row of
 *  push-buttons. Buttons carry a hotkey (the letter after '&', drawn in red);
 *  Left/Right or Tab move the focus, Enter/Space press the focused button, the
 *  hotkey letter presses it directly, Esc cancels.
 */
static int dlg_up(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static int dlg_disp_len(const char *s) { int n = 0; for (; *s; s++) if (*s != '&') n++; return n; }
static int dlg_btn_w(const char *s)    { return dlg_disp_len(s) + 4; }   /* "[ " label " ]" */
static int dlg_hotkey(const char *s)   { for (; *s; s++) if (*s == '&' && s[1]) return s[1]; return 0; }

/* A filled, bordered box with an optional title in the top edge. */
static void draw_box(int x, int y, int w, int h, const char *title)
{
    int r;
    bg(C_WHITE); fg(C_BLACK);
    for (r = 0; r < h; r++) repeat_ch(x, y + r, ' ', w);
    at(x, y); out(B_TL); repeat_ch(x + 1, y, B_H, w - 2); at(x + w - 1, y); out(B_TR);
    for (r = 1; r < h - 1; r++) { at(x, y + r); out(B_V); at(x + w - 1, y + r); out(B_V); }
    at(x, y + h - 1); out(B_BL); repeat_ch(x + 1, y + h - 1, B_H, w - 2); at(x + w - 1, y + h - 1); out(B_BR);
    if (title && *title) {
        int tl = (int)strlen(title), tx = x + (w - tl - 2) / 2;
        at(tx, y); out(' '); puts_at(tx + 1, y, title); out(' ');
    }
}

static void draw_button(int x, int y, const char *label, int focused)
{
    const char *p;
    bg(focused ? C_CYAN : C_WHITE); fg(C_BLACK);
    at(x, y); out('['); out(' ');
    for (p = label; *p; p++) {
        if (*p == '&') { if (p[1]) { fg(C_RED); out((unsigned char)p[1]); fg(C_BLACK); p++; } }
        else out((unsigned char)*p);
    }
    out(' '); out(']');
}

/* Show the dialog; returns the pressed button's index, or -1 on Esc. */
static int dialog(const char *title, const char *msg, const char *const *btns, int nbtn, int defbtn)
{
    int i, msgw = (int)strlen(msg), brow = 0, innerw, w, h, x, y, focus = defbtn;
    int bw[6];
    if (nbtn > 6) nbtn = 6;
    for (i = 0; i < nbtn; i++) { bw[i] = dlg_btn_w(btns[i]); brow += bw[i] + 1; }
    if (nbtn > 0) brow -= 1;
    innerw = msgw > brow ? msgw : brow;
    w = innerw + 6; if (w > COLS - 2) w = COLS - 2; if (w < 14) w = 14;
    h = 6; x = (COLS - w) / 2; y = (ROWS - h) / 2;

    redraw();                                        /* editor behind the box */
    for (;;) {
        int c, mods, bx, by;
        draw_box(x, y, w, h, title);
        bg(C_WHITE); fg(C_BLACK);
        puts_at(x + (w - msgw) / 2, y + 2, msg);
        bx = x + (w - brow) / 2; by = y + h - 2;
        for (i = 0; i < nbtn; i++) { draw_button(bx, by, btns[i], i == focus); bx += bw[i] + 1; }

        c = key();
        mods = S->keymods();
        if (c == K_ESC) { redraw(); return -1; }
        else if (c == K_LEFT  || (c == K_TAB && (mods & KMOD_SHIFT))) focus = (focus + nbtn - 1) % nbtn;
        else if (c == K_RIGHT ||  c == K_TAB)                         focus = (focus + 1) % nbtn;
        else if (c == K_ENTER || c == ' ') { redraw(); return focus; }
        else for (i = 0; i < nbtn; i++) {
            int hk = dlg_hotkey(btns[i]);
            if (hk && dlg_up(c) == dlg_up(hk)) { redraw(); return i; }
        }
    }
}

/* A one-button information/error box. */
static void message(const char *msg)
{
    static const char *ok[] = { "&OK" };
    dialog("ed", msg, ok, 1, 0);
}

/* Read a line of text into buf: a titled input box with [ OK ] / [ Cancel ].
 * Tab cycles the field and the two buttons; Enter accepts, Esc cancels.
 * Returns 1 if OK'd with a non-empty entry, 0 if cancelled. */
static int prompt(const char *label, char *buf, int max)
{
    int w = COLS / 2, x, y, n = 0, focus = 0;   /* focus: 0 field, 1 OK, 2 Cancel */
    static const char *btns[] = { "&OK", "&Cancel" };
    int bw0 = dlg_btn_w(btns[0]), bw1 = dlg_btn_w(btns[1]), h = 7;
    if (w < 30) w = 30; if (w > COLS - 2) w = COLS - 2;
    x = (COLS - w) / 2; y = (ROWS - h) / 2;
    buf[0] = 0;

    redraw();
    for (;;) {
        int c, brow = bw0 + 1 + bw1, bx, by;
        draw_box(x, y, w, h, "ed");
        bg(C_WHITE); fg(C_BLACK);
        puts_at(x + 2, y + 1, label);
        bg(focus == 0 ? C_CYAN : C_WHITE); fg(C_BLACK);        /* the input field */
        repeat_ch(x + 2, y + 3, ' ', w - 4);
        puts_at(x + 2, y + 3, buf);
        if (focus == 0) caret(x + 2 + n, y + 3);
        bg(C_WHITE); fg(C_BLACK);
        bx = x + (w - brow) / 2; by = y + h - 2;
        draw_button(bx, by, btns[0], focus == 1);
        draw_button(bx + bw0 + 1, by, btns[1], focus == 2);

        c = key();
        if (c == K_ESC) { redraw(); return 0; }
        if (c == K_ENTER) { redraw(); return focus == 2 ? 0 : (n > 0); }   /* Enter: field/OK accept, Cancel cancels */
        if (c == K_TAB) { focus = (focus + 1) % 3; continue; }
        if (dlg_up(c) == 'O' && focus != 0) { redraw(); return n > 0; }
        if (dlg_up(c) == 'C' && focus != 0) { redraw(); return 0; }
        if (focus == 0) {                                      /* editing the field */
            if (c == K_BS) { if (n) buf[--n] = 0; continue; }
            if (c >= 32 && c < 127 && n < max - 1) { buf[n++] = (char)c; buf[n] = 0; }
        } else {                                               /* on a button */
            if (c == ' ') { redraw(); return focus == 2 ? 0 : (n > 0); }
            if (c == K_LEFT)  focus = focus == 2 ? 1 : 0;
            if (c == K_RIGHT) focus = focus == 1 ? 2 : (focus == 0 ? 1 : 2);
        }
    }
}

/* A drop-down list. Returns the chosen index, or one of the NAV_* sentinels
 * (Esc / Left / Right) so the caller can move along the menu bar. Does not
 * redraw; the caller does that once the menu is dismissed. */
#define NAV_CANCEL (-1)
#define NAV_LEFT   (-2)
#define NAV_RIGHT  (-3)

/* A menu item that is just "-" is a non-selectable separator. */
static int is_sep(const char *s) { return s[0] == '-' && s[1] == 0; }

static int popup(int x, int y, const char *const *items, int n)
{
    int sel = 0, i, c, w = 0;

    for (i = 0; i < n; i++) {
        int l = is_sep(items[i]) ? 0 : (int)strlen(items[i]);
        if (l > w) w = l;
    }
    w += 2;
    while (sel < n && is_sep(items[sel])) sel++;     /* never start on a separator */

    cursor(0);
    for (;;) {
        for (i = 0; i < n; i++) {
            if (is_sep(items[i])) {
                bg(C_WHITE); fg(C_BLACK);
                at(x, y + i);
                for (int j = 0; j < w; j++) out(B_H);        /* a horizontal rule */
            } else {
                bg(i == sel ? C_CYAN : C_WHITE); fg(C_BLACK);
                repeat_ch(x, y + i, ' ', w);
                puts_at(x + 1, y + i, items[i]);
            }
        }
        c = key();
        if (c == K_UP)    { do { sel = sel ? sel - 1 : n - 1; } while (is_sep(items[sel])); continue; }
        if (c == K_DOWN)  { do { sel = (sel + 1) % n;         } while (is_sep(items[sel])); continue; }
        if (c == K_ENTER) { if (!is_sep(items[sel])) return sel; continue; }
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

    /* horizontal (the gutter narrows the usable text width; column is measured
     * within the body, so the hidden BASIC line number does not consume width) */
    {
        int vcol = curx - body_off();
        if (vcol < coloff) coloff = vcol;
        if (vcol - coloff > win_x2 - win_x1 - gutter_w) coloff = vcol - (win_x2 - win_x1 - gutter_w);
        if (coloff < 0) coloff = 0;
    }

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

/* Vertical moves keep the *visual* column (position within the code body), so
 * the caret stays put over the code even when adjacent lines have line numbers
 * of different widths. With body_off==0 (non-BASIC) this is the old behaviour. */
static void go_up(void)
{
    int vcol;
    if (!cur->prev) return;
    vcol = curx - body_off();
    cur = cur->prev; lineno--;
    curx = body_off() + (vcol < 0 ? 0 : vcol);
    if (curx > cur->len) curx = cur->len;
}

static void go_down(void)
{
    int vcol;
    if (!cur->next) return;
    vcol = curx - body_off();
    cur = cur->next; lineno++;
    curx = body_off() + (vcol < 0 ? 0 : vcol);
    if (curx > cur->len) curx = cur->len;
}

/* ------------------------------------------------------------- EDITING */

static void mark_changed(void) { changed = 1; }

/* ---------------------------------------------------------- UNDO / REDO */
/* Snapshot based: before each edit the whole buffer (serialised to text) plus
 * the cursor position is pushed on the undo stack. Undo/redo swap snapshots
 * between the two stacks and rebuild the line list. Simple and correct for every
 * operation - typing, delete, split/join, auto-renumber, replace-all - because
 * it never has to invert individual edits. Snapshots are malloc'd from the seed
 * heap, which RUN (F5) wipes, so the stacks are dropped (undo_forget) after a
 * run. Runs of typed characters coalesce into one undo step. */
#define UNDO_MAX 48

typedef struct { char *text; int ci, cx, ti, co; } Snapshot;
static Snapshot undo_stk[UNDO_MAX], redo_stk[UNDO_MAX];
static int undo_n, redo_n;
static int undo_group;              /* 1 while coalescing a run of typed characters */

/* The whole buffer as one malloc'd string, lines joined with '\n'. */
static char *undo_serialize(void)
{
    int total = 1, n = 0; Line *p; char *s;
    for (p = first; p; p = p->next) total += p->len + 1;
    s = (char *)malloc((size_t)total);
    if (!s) return 0;
    for (p = first; p; p = p->next) {
        if (p != first) s[n++] = '\n';
        memcpy(s + n, p->s, (size_t)p->len); n += p->len;
    }
    s[n] = 0;
    return s;
}

/* Push the current buffer + cursor onto stack stk (capped; drops the oldest). */
static void undo_snap(Snapshot *stk, int *n)
{
    char *t = undo_serialize();
    int i;
    if (!t) return;                          /* out of memory: just skip this step */
    if (*n >= UNDO_MAX) {
        free(stk[0].text);
        for (i = 1; i < UNDO_MAX; i++) stk[i - 1] = stk[i];
        (*n)--;
    }
    stk[*n].text = t;
    stk[*n].ci = line_index(cur); stk[*n].cx = curx;
    stk[*n].ti = line_index(top); stk[*n].co = coloff;
    (*n)++;
}

static void redo_drop(void)
{
    int i;
    for (i = 0; i < redo_n; i++) free(redo_stk[i].text);
    redo_n = 0;
}

/* Called before a discrete edit; a new edit invalidates the redo history. */
static void undo_record(void) { undo_snap(undo_stk, &undo_n); redo_drop(); undo_group = 0; }

/* Called before a typed character: only the first of a run snapshots, so undo
 * removes the whole run at once. The run is broken by any non-typing key (see
 * the main loop). */
static void undo_record_typing(void)
{
    if (!undo_group) { undo_snap(undo_stk, &undo_n); redo_drop(); }
    undo_group = 1;
}

/* Rebuild the line list from a text buffer (like load_file, but from memory). */
static void undo_rebuild(const char *text)
{
    Line *ln;
    free_all();
    first = line_alloc();
    ln = first;
    while (*text && ln) {
        char c = *text++;
        if (c == '\n') ln = line_insert_after(ln, 0);
        else if (ln->len < MAXLEN) { ln->s[ln->len++] = c; ln->s[ln->len] = 0; }
    }
}

static void undo_apply(Snapshot *s)
{
    undo_rebuild(s->text);
    cur = line_at(s->ci); top = line_at(s->ti);
    curx = s->cx; coloff = s->co; lineno = s->ci + 1;
    sel_anchor = 0;
    if (curx > cur->len) curx = cur->len;
    if (curx < body_off()) curx = body_off();
    scroll_into_view();
    mark_changed();
}

static void do_undo(void)
{
    if (undo_n == 0) { message("Nothing to undo."); return; }
    undo_snap(redo_stk, &redo_n);            /* current -> redo */
    undo_n--;
    undo_apply(&undo_stk[undo_n]);
    free(undo_stk[undo_n].text); undo_stk[undo_n].text = 0;
    undo_group = 0;
}

static void do_redo(void)
{
    if (redo_n == 0) { message("Nothing to redo."); return; }
    undo_snap(undo_stk, &undo_n);            /* current -> undo (redo history kept) */
    redo_n--;
    undo_apply(&redo_stk[redo_n]);
    free(redo_stk[redo_n].text); redo_stk[redo_n].text = 0;
    undo_group = 0;
}

/* Drop the stacks WITHOUT freeing - the seed heap that held them was reset. */
static void undo_forget(void) { undo_n = 0; redo_n = 0; undo_group = 0; }

/* Free the stacks and drop them (the buffer is still valid, e.g. before opening
 * another file). */
static void undo_clear(void)
{
    int i;
    for (i = 0; i < undo_n; i++) free(undo_stk[i].text);
    for (i = 0; i < redo_n; i++) free(redo_stk[i].text);
    undo_forget();
}

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
    undo_record_typing();

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
    int i, join, lo = body_off();

    if (curx > lo) {
        undo_record();
        for (i = curx - 1; i < cur->len; i++) cur->s[i] = cur->s[i + 1];
        cur->len--; curx--;
        mark_changed();
        return;
    }
    if (lo > 0) return;                 /* at the code start: the line number is protected */
    if (!cur->prev) return;

    /* join this line onto the end of the previous one */
    if (cur->prev->len + cur->len > MAXLEN) {
        message("Joined line would be too long.");
        return;
    }
    undo_record();
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
        undo_record();
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
    undo_record();
    memcpy(cur->s + cur->len, cur->next->s, cur->next->len);
    cur->len += cur->next->len;
    cur->s[cur->len] = 0;
    line_remove(cur->next);
    mark_changed();
}

/* ===================== AUTO LINE NUMBERING (BASIC) =====================
 * When Auto Numbering is on, the BASIC line number is managed for the user: it
 * shows in the gutter, is never edited by hand, and Enter assigns the new line a
 * free number between its neighbours. When the neighbours are adjacent (no free
 * number between them) the whole program is renumbered to a step of 10 and every
 * line-number *reference* (GOTO/GOSUB/THEN/ELSE/RESTORE, incl. ON..GOTO lists)
 * is rewritten to match. Turning the feature off drops the gutter and lets the
 * programmer manage numbers as plain (still syntax-coloured) text. */

/* The leading decimal number of a line, or -1 if it has none. */
static int line_num(Line *p)
{
    int i = 0, v = 0;
    if (!p) return -1;
    while (i < p->len && p->s[i] >= '0' && p->s[i] <= '9') { v = v * 10 + (p->s[i] - '0'); i++; }
    return i ? v : -1;
}

/* Characters occupied by "digits + one space" - mode-independent (body_off_of is
 * the mode-gated version used for the cursor/rendering). */
static int num_prefix_len(Line *p)
{
    int i = lead_digits(p);
    if (i == 0) return 0;
    if (i < p->len && p->s[i] == ' ') i++;
    return i;
}

/* Keywords a line number may follow. Matched whole-word, case-insensitive. */
static int is_ref_kw(const char *w, int len)
{
    static const char *const r[] = { "GOTO", "GOSUB", "THEN", "ELSE", "RESTORE", 0 };
    int k, j;
    for (k = 0; r[k]; k++) {
        for (j = 0; j < len && r[k][j] && upc((unsigned char)w[j]) == r[k][j]; j++) ;
        if (j == len && r[k][j] == 0) return 1;
    }
    return 0;
}

/* Rewrite code `s` (length len) into `out`, remapping every line-number
 * reference through old[]->new[] (cnt entries). Bare numbers that are data, and
 * anything inside a string literal, are copied unchanged. Returns the output
 * length, or -1 on overflow. */
static int fix_refs(const char *s, int len, const int *oldn, const int *newn, int cnt,
                    char *out, int outmax)
{
    int i = 0, o = 0, ovf = 0;
#define PUT(ch) do { if (o < outmax) out[o++] = (char)(ch); else ovf = 1; } while (0)
#define PUTNUM(v) do { char rv[12]; int nd_ = 0, vv_ = (v), t_; \
        if (vv_ <= 0) rv[nd_++] = '0'; else while (vv_) { rv[nd_++] = (char)('0' + vv_ % 10); vv_ /= 10; } \
        for (t_ = 0; t_ < nd_; t_++) PUT(rv[nd_ - 1 - t_]); } while (0)

    while (i < len) {
        char c = s[i];
        if (c == '"') {                                   /* string: verbatim */
            PUT(c); i++;
            while (i < len && s[i] != '"') { PUT(s[i]); i++; }
            if (i < len) { PUT(s[i]); i++; }
        } else if (is_alpha((unsigned char)c)) {          /* word */
            int j = i, k;
            while (j < len && (is_alpha((unsigned char)s[j]) || is_digit((unsigned char)s[j]))) j++;
            for (k = i; k < j; k++) PUT(s[k]);
            if (is_ref_kw(s + i, j - i)) {                /* a reference follows */
                i = j;
                { int si = i, so = o;                     /* spaces before the first number */
                  while (i < len && s[i] == ' ') { PUT(s[i]); i++; }
                  if (!(i < len && is_digit((unsigned char)s[i]))) { i = si; o = so; } }
                while (i < len && is_digit((unsigned char)s[i])) {
                    int r = 0, nn, t;
                    while (i < len && is_digit((unsigned char)s[i])) { r = r * 10 + (s[i] - '0'); i++; }
                    nn = r;
                    for (t = 0; t < cnt; t++) if (oldn[t] == r) { nn = newn[t]; break; }
                    PUTNUM(nn);
                    { int si = i, so = o;                 /* a comma continues an ON..GOTO list */
                      while (i < len && s[i] == ' ') { PUT(s[i]); i++; }
                      if (i < len && s[i] == ',') {
                          PUT(','); i++;
                          while (i < len && s[i] == ' ') { PUT(s[i]); i++; }
                      } else { i = si; o = so; break; }
                    }
                }
            } else {
                i = j;
            }
        } else if (is_digit((unsigned char)c)) {          /* bare number = data */
            while (i < len && is_digit((unsigned char)s[i])) { PUT(s[i]); i++; }
        } else {
            PUT(c); i++;
        }
    }
#undef PUTNUM
#undef PUT
    return ovf ? -1 : o;
}

/* Prepend "num " to p, treating ALL of p's current text as code (never stripping
 * a leading number - a freshly split line is pure code that may itself begin with
 * digits). Returns -1 on overflow. */
static int line_prepend_number(Line *p, int num)
{
    char tmp[MAXLEN + 1], rev[12];
    int nd = 0, vv = num, k, total = 0;
    if (vv <= 0) rev[nd++] = '0'; else while (vv) { rev[nd++] = (char)('0' + vv % 10); vv /= 10; }
    total = nd + 1 + p->len;
    if (total > MAXLEN) return -1;
    for (k = 0; k < nd; k++) tmp[k] = rev[nd - 1 - k];
    tmp[nd] = ' ';
    memcpy(tmp + nd + 1, p->s, p->len);
    memcpy(p->s, tmp, total); p->len = total; p->s[total] = 0;
    return 0;
}

/* Set p's number to newnum AND rewrite its references (used by renumber). */
static int line_rebuild(Line *p, int newnum, const int *oldn, const int *newn, int cnt)
{
    char code[MAXLEN + 1], fixed[MAXLEN * 2 + 2], tmp[MAXLEN + 1], rev[12];
    int pl = num_prefix_len(p), clen = p->len - pl, flen, nd = 0, vv = newnum, k, total;
    memcpy(code, p->s + pl, clen);
    flen = fix_refs(code, clen, oldn, newn, cnt, fixed, (int)sizeof(fixed));
    if (flen < 0) return -1;
    if (vv <= 0) rev[nd++] = '0'; else while (vv) { rev[nd++] = (char)('0' + vv % 10); vv /= 10; }
    total = nd + 1 + flen;
    if (total > MAXLEN) return -1;
    for (k = 0; k < nd; k++) tmp[k] = rev[nd - 1 - k];
    tmp[nd] = ' ';
    memcpy(tmp + nd + 1, fixed, flen);
    memcpy(p->s, tmp, total); p->len = total; p->s[total] = 0;
    return 0;
}

#define RN_MAX 4096

/* Largest line number in the program (0 if none). */
static int max_line_num(void)
{
    int m = 0; Line *p;
    for (p = first; p; p = p->next) { int n = line_num(p); if (n > m) m = n; }
    return m;
}

/* Renumber from `start` to the end at `step`, leaving every earlier line's number
 * untouched - the change stays local to the insertion point. New numbers continue
 * from the last kept line (start->prev). References are fixed across the WHOLE
 * program (an earlier line may GOTO one that moved), so both halves below run
 * line_rebuild with the same old->new map. */
static int renumber_from(Line *start, int step)
{
    static int oldn[RN_MAX], newn[RN_MAX];
    int cnt = 0, base, idx; Line *p;
    base = start->prev ? line_num(start->prev) : 0;
    for (p = start; p; p = p->next) {
        if (cnt >= RN_MAX) { message("Too many lines to renumber."); return -1; }
        oldn[cnt] = line_num(p);
        newn[cnt] = base + (cnt + 1) * step;
        cnt++;
    }
    for (p = first; p != start; p = p->next)          /* earlier lines: keep number, fix refs */
        if (line_rebuild(p, line_num(p), oldn, newn, cnt) < 0) {
            message("Renumber failed (line too long)."); return -1;
        }
    idx = 0;
    for (p = start; p; p = p->next, idx++)             /* moved lines: new number + fix refs */
        if (line_rebuild(p, newn[idx], oldn, newn, cnt) < 0) {
            message("Renumber failed (line too long)."); return -1;
        }
    mark_changed();
    return 0;
}

/* Assign nl a number between its neighbours; if the gap is exhausted, renumber
 * from nl onwards (earlier lines keep their numbers, references fixed globally). */
static void autonum_assign(Line *nl)
{
    int prev = nl->prev ? line_num(nl->prev) : -1;
    int nxt  = nl->next ? line_num(nl->next) : -1;
    int lo = prev >= 0 ? prev : 0, newn = 0, room = 1;
    if (nxt >= 0) {
        if (nxt - lo >= 2) newn = lo + (nxt - lo) / 2;
        else room = 0;                                /* no free number between neighbours */
    } else {
        newn = lo + 10;
    }
    if (room) {
        if (line_prepend_number(nl, newn) < 0) message("Line too long to number.");
    } else {
        /* Give nl a unique, unreferenced provisional number (above every real one)
         * so the renumber's old->new map has no collision - a real line whose code
         * starts with digits would otherwise be ambiguous - then renumber the tail. */
        if (line_prepend_number(nl, max_line_num() + 10) < 0) { message("Line too long to number."); return; }
        renumber_from(nl, 10);
    }
}

/* Make sure every line carries a number (called when auto mode turns on). */
static void autonum_ensure(void)
{
    Line *p;
    for (p = first; p; p = p->next)
        if (line_num(p) < 0) autonum_assign(p);
}

static void toggle_autonum(void)
{
    if (!is_basic) { message("Auto numbering applies to BASIC (.BAS) files."); return; }
    undo_record();
    auto_num = !auto_num;
    if (auto_num) autonum_ensure();
    coloff = 0;
    if (curx < body_off()) curx = body_off();
    mark_changed();
    message(auto_num ? "Auto numbering ON." : "Auto numbering OFF - manual line numbers.");
}

static void do_enter(void)
{
    Line *nl;

    undo_record();
    while (curx > cur->len) cur->s[cur->len++] = ' ';
    cur->s[cur->len] = 0;

    nl = line_insert_after(cur, cur->s + curx);
    if (!nl) { message("Out of memory."); return; }

    cur->s[curx] = 0;
    cur->len = curx;
    cur = nl;
    lineno++;
    coloff = 0;
    if (is_basic && auto_num) {
        autonum_assign(nl);         /* give the new line a managed number */
        curx = body_off();          /* cursor at the start of its code */
    } else {
        curx = 0;
    }
    mark_changed();
}

static void do_tab(void)
{
    int i, n = TABSIZE - (curx % TABSIZE);
    for (i = 0; i < n; i++) insert_char(' ');
}

/* -------------------------------------------------------- FILE DIALOG
 *
 *  A browsing Open / Save-As dialog over the pod-libc <dirent.h>: it lists the
 *  current directory (".." then sub-directories then files), lets you walk in
 *  and out of folders, type a name, and make a new folder (F7).
 */
#define FD_MAX   400        /* most entries shown for one directory */
#define FD_NAMEW 64         /* stored/displayed name width          */

typedef struct { char name[FD_NAMEW]; int is_dir; long size; } fd_entry;
static fd_entry fd_list[FD_MAX];
static int      fd_n;
static char     fd_dir[256];        /* current browse directory (absolute) */

static int fd_less(const fd_entry *a, const fd_entry *b)   /* dirs first, then name */
{
    const char *x, *y;
    if (a->is_dir != b->is_dir) return a->is_dir > b->is_dir;
    for (x = a->name, y = b->name; *x && *y; x++, y++) {
        int cx = (*x >= 'a' && *x <= 'z') ? *x - 32 : *x;
        int cy = (*y >= 'a' && *y <= 'z') ? *y - 32 : *y;
        if (cx != cy) return cx < cy;
    }
    return (unsigned char)*x < (unsigned char)*y;
}

static int fd_ci_eq(const char *a, const char *b)   /* case-insensitive equality */
{
    for (; *a && *b; a++, b++) {
        int ca = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a;
        int cb = (*b >= 'a' && *b <= 'z') ? *b - 32 : *b;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static void fd_pick(char *name, int sel)   /* copy entry `sel`'s name into name[] */
{
    int k = 0;
    if (sel >= 0 && sel < fd_n)
        for (; fd_list[sel].name[k] && k < FD_NAMEW - 1; k++) name[k] = fd_list[sel].name[k];
    name[k] = 0;
}

static void fd_scan(void)           /* fill fd_list from fd_dir */
{
    DIR *d;
    struct dirent *e;
    int i;

    fd_n = 0;
    if (!(fd_dir[0] == '/' && fd_dir[1] == 0)) {                 /* ".." unless at root */
        strcpy(fd_list[0].name, ".."); fd_list[0].is_dir = 1; fd_list[0].size = 0; fd_n = 1;
    }
    d = opendir(fd_dir);
    if (d) {
        while ((e = readdir(d)) && fd_n < FD_MAX) {
            if (e->d_name[0] == '.' &&
                (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
            { int k = 0; for (; e->d_name[k] && k < FD_NAMEW - 1; k++) fd_list[fd_n].name[k] = e->d_name[k];
              fd_list[fd_n].name[k] = 0; }
            fd_list[fd_n].is_dir = (e->d_type == DT_DIR);
            fd_list[fd_n].size   = e->d_size;
            fd_n++;
        }
        closedir(d);
    }
    for (i = 1; i < fd_n; i++) {                                 /* insertion sort */
        fd_entry t = fd_list[i]; int j = i - 1;
        while (j >= 0 && !fd_less(&fd_list[j], &t)) { fd_list[j + 1] = fd_list[j]; j--; }
        fd_list[j + 1] = t;
    }
}

static void fd_enter(const char *name)      /* navigate into `name` (or ".." up) */
{
    int n = (int)strlen(fd_dir);
    if (!strcmp(name, "..")) {
        while (n > 0 && fd_dir[n - 1] == '/') n--;              /* strip trailing '/'   */
        while (n > 0 && fd_dir[n - 1] != '/') n--;              /* drop last component  */
        if (n <= 1) { fd_dir[0] = '/'; fd_dir[1] = 0; } else fd_dir[n - 1] = 0;
        return;
    }
    if (n && fd_dir[n - 1] != '/') fd_dir[n++] = '/';
    for (int i = 0; name[i] && n < (int)sizeof(fd_dir) - 1; i++) fd_dir[n++] = name[i];
    fd_dir[n] = 0;
}

static void fd_path(const char *name, char *out, int outsz)   /* fd_dir + "/" + name */
{
    int n = 0;
    for (int i = 0; fd_dir[i] && n < outsz - 1; i++) out[n++] = fd_dir[i];
    if (n && out[n - 1] != '/' && n < outsz - 1) out[n++] = '/';
    for (int i = 0; name[i] && n < outsz - 1; i++) out[n++] = name[i];
    out[n] = 0;
}

/* Browse for a file. `save` shows the name field pre-filled for a new name.
 * Returns 1 with an absolute path in `result`, 0 if cancelled. */
static int file_dialog(const char *title, int save, char *result, int ressz)
{
    char name[FD_NAMEW];
    int sel = 0, top = 0, edited = 0;   /* edited: user has typed into the name field */
    int bw, bh, bx, by, listrows;

    if (fd_dir[0] == 0) { if (getcwd(fd_dir, sizeof fd_dir) == 0 || fd_dir[0] == 0) { fd_dir[0] = '/'; fd_dir[1] = 0; } }
    fd_scan();
    if (save) {                                    /* pre-fill with the file's basename */
        const char *b = filename, *p;
        for (p = filename; *p; p++) if (*p == '/') b = p + 1;
        { int k = 0; for (; b[k] && k < FD_NAMEW - 1; k++) name[k] = b[k]; name[k] = 0; }
    } else fd_pick(name, 0);

    bw = COLS - 8; if (bw > 64) bw = 64; if (bw < 30) bw = 30;
    bh = ROWS - 6; if (bh > 22) bh = 22; if (bh < 10) bh = 10;
    bx = (COLS - bw) / 2; by = (ROWS - bh) / 2;
    listrows = bh - 6;

    for (;;) {
        int r, idx;
        if (sel >= fd_n) sel = fd_n - 1;
        if (sel < 0) sel = 0;
        if (sel < top) top = sel;
        if (sel >= top + listrows) top = sel - listrows + 1;
        if (top < 0) top = 0;

        redraw();                                  /* editor behind, erase old dialog */
        bg(C_WHITE); fg(C_BLACK);
        for (r = 0; r < bh; r++) repeat_ch(bx, by + r, ' ', bw);
        puts_at(bx + 2, by, title);
        puts_at(bx + 2, by + 1, "Dir: ");
        { const char *dp = fd_dir; int dl = (int)strlen(dp), room = bw - 8;
          if (dl > room) dp += dl - room;
          puts_at(bx + 7, by + 1, dp); }

        for (r = 0; r < listrows; r++) {
            int ly = by + 3 + r;
            idx = top + r;
            bg(idx == sel && idx < fd_n ? C_CYAN : C_WHITE); fg(C_BLACK);
            repeat_ch(bx + 2, ly, ' ', bw - 4);
            if (idx < fd_n) {
                const char *q;
                at(bx + 3, ly);
                if (fd_list[idx].is_dir) { for (q = "[DIR] "; *q; q++) out((unsigned char)*q); }
                else                     { int s; for (s = 0; s < 6; s++) out(' '); }
                for (q = fd_list[idx].name; *q; q++) out((unsigned char)*q);
            }
        }

        bg(C_WHITE); fg(C_BLACK);
        puts_at(bx + 2, by + bh - 2, "Name: ");
        repeat_ch(bx + 8, by + bh - 2, ' ', bw - 10);
        puts_at(bx + 8, by + bh - 2, name);
        caret(bx + 8 + (int)strlen(name), by + bh - 2);
        puts_at(bx + 2, by + bh - 1, save ? " Enter save  F7 new folder  Esc cancel "
                                          : " Enter open  F7 new folder  Esc cancel ");

        int c = key();
        if (c == K_ESC) { redraw(); return 0; }
        /* Navigation refills the name field from the selection (edited=0); the
         * first keystroke afterwards starts a fresh name. */
        if (c == K_UP)   { if (sel > 0)        { sel--; edited = 0; fd_pick(name, sel); } continue; }
        if (c == K_DOWN) { if (sel < fd_n - 1) { sel++; edited = 0; fd_pick(name, sel); } continue; }
        if (c == K_PGUP) { sel -= listrows; if (sel < 0) sel = 0; edited = 0; fd_pick(name, sel); continue; }
        if (c == K_PGDN) { sel += listrows; if (sel > fd_n - 1) sel = fd_n - 1; edited = 0; fd_pick(name, sel); continue; }
        if (c == K_F7) {
            char nm[FD_NAMEW]; nm[0] = 0;
            if (prompt("New folder name?", nm, sizeof nm) && nm[0]) {
                char p[256]; fd_path(nm, p, sizeof p);
                if (mkdir(p, 0777) != 0) message("Could not create the folder.");
                fd_scan(); sel = 0; top = 0; edited = 0; fd_pick(name, 0);
            }
            continue;
        }
        if (c == K_ENTER) {
            int di = -1, i;                       /* does the name field name a directory? */
            for (i = 0; i < fd_n; i++)
                if (fd_list[i].is_dir && fd_ci_eq(fd_list[i].name, name)) { di = i; break; }
            if (di >= 0) {                        /* yes: walk into it */
                fd_enter(fd_list[di].name);
                fd_scan(); sel = 0; top = 0; edited = 0; fd_pick(name, 0);
                continue;
            }
            if (name[0]) { fd_path(name, result, ressz); redraw(); return 1; }
            continue;                             /* empty name: do nothing */
        }
        if (c == K_BS) {
            if (!edited) { name[0] = 0; edited = 1; }   /* first edit starts fresh */
            else { int l = (int)strlen(name); if (l) name[l - 1] = 0; }
            continue;
        }
        if (c >= 32 && c < 127) {
            int l;
            if (!edited) { name[0] = 0; edited = 1; }   /* first char replaces the field */
            l = (int)strlen(name);
            if (l < FD_NAMEW - 1) { name[l] = (char)c; name[l + 1] = 0; }
            continue;
        }
    }
}

/* --------------------------------------------------------- MENU ACTIONS */

static void help_screen(void);          /* defined below; used by the Help menu */

static void do_new(void)
{
    undo_clear(); nav_reset();
    free_all();
    first = line_alloc();
    top = cur = first;
    curx = coloff = 0;
    lineno = 1;
    changed = 0;
    strcpy(filename, "NONAME.TXT");
}

/* Open / Save / Save As, driven by the browsing file dialog. */
static void do_open(void)
{
    char path[256];
    if (file_dialog("Open", 0, path, sizeof path)) {
        if (load_file(path) < 0) message("Could not open that file.");
        else { undo_clear(); nav_reset(); strcpy(filename, path); }
    }
}
static void do_saveas(void)
{
    char path[256];
    if (file_dialog("Save As", 1, path, sizeof path)) {
        if (save_file(path) < 0) message("Could not save.");
        else { strcpy(filename, path); message("Saved."); }
    }
}
static void do_save(void)
{
    if (!strcmp(filename, "NONAME.TXT")) { do_saveas(); return; }   /* never named yet */
    if (save_file(filename) < 0) message("Could not save.");
    else message("Saved.");
}

/* ------------------------------------------------------------ SEARCH */

static char find_s[128];        /* current search string (empty = none yet) */
static char repl_s[128];        /* current replacement string */
static int  find_len;           /* strlen(find_s), cached */

/* Case-insensitive byte compare (BASIC is case-insensitive; nicer for prose too). */
static int ci_eq(char a, char b)
{
    if (a >= 'a' && a <= 'z') a -= 32;
    if (b >= 'a' && b <= 'z') b -= 32;
    return a == b;
}

/* Does find_s occur in p->s starting exactly at column i? */
static int match_at(Line *p, int i)
{
    int k;
    if (i + find_len > p->len) return 0;
    for (k = 0; k < find_len; k++) if (!ci_eq(p->s[i + k], find_s[k])) return 0;
    return 1;
}

/* Find the next occurrence of find_s at or after (from, fcol). On success moves
 * the cursor to the match end, selects the match, scrolls it into view and
 * returns 1; otherwise returns 0. In BASIC auto-number mode the hidden line
 * number is skipped so matches land in the code. */
static int search_from(Line *from, int fcol)
{
    Line *p;
    for (p = from; p; p = p->next) {
        int bo = body_off_of(p);
        int i  = (p == from && fcol > bo) ? fcol : bo;
        for (; i + find_len <= p->len; i++) {
            if (match_at(p, i)) {
                sel_anchor = p; sel_acol = i;          /* highlight the match */
                cur = p; curx = i + find_len;
                lineno = line_index(p) + 1;
                scroll_into_view();
                return 1;
            }
        }
    }
    return 0;
}

/* Search forward from just after the cursor, wrapping once to the top. */
static int search_forward(void)
{
    return search_from(cur, curx + 1) || search_from(first, 0);
}

/* Report "<verb> N occurrence(s)." in a message box. */
static void report_hits(const char *verb, int n)
{
    char m[48]; int k = 0; const char *q;
    for (q = verb; *q; q++) m[k++] = *q;
    k += fmt_uint(m + k, n);
    for (q = (n == 1) ? " occurrence." : " occurrences."; *q; q++) m[k++] = *q;
    m[k] = 0;
    message(m);
}

/* Find... : prompt for a string, then jump to the next occurrence. */
static void do_find(void)
{
    if (!prompt("Find:", find_s, sizeof find_s)) return;
    find_len = (int)strlen(find_s);
    if (!find_len) return;
    if (!search_forward()) message("Not found.");
}

/* Find Next : repeat the last search (prompting first if there is none). */
static void find_next(void)
{
    if (!find_len) { do_find(); return; }
    if (!search_forward()) message("Not found.");
}

/* Replace... : prompt for the search and replacement, then replace every
 * occurrence (case-insensitive) and report the count. The replacement must be
 * non-empty (an empty field is treated as cancel). */
static void do_replace(void)
{
    int rlen, count = 0;
    Line *p;
    if (!prompt("Replace - find:", find_s, sizeof find_s)) return;
    find_len = (int)strlen(find_s);
    if (!find_len) return;
    if (!prompt("Replace - with:", repl_s, sizeof repl_s)) return;
    rlen = (int)strlen(repl_s);

    undo_record();
    for (p = first; p; p = p->next) {
        int i = body_off_of(p);
        while (i + find_len <= p->len) {
            if (match_at(p, i)) {
                if (p->len - find_len + rlen > MAXLEN) { i++; continue; }  /* would overflow */
                memmove(p->s + i + rlen, p->s + i + find_len,
                        (size_t)(p->len - (i + find_len) + 1));            /* incl. NUL */
                memcpy(p->s + i, repl_s, (size_t)rlen);
                p->len += rlen - find_len;
                i += rlen; count++;
            } else i++;
        }
    }
    if (count) mark_changed();
    sel_anchor = 0;
    if (curx > cur->len) curx = cur->len;
    report_hits("Replaced ", count);
}

/* ------------------------------------------ BASIC LINE-REFERENCE NAV */
/* BASIC control flow is line-number based (GOTO/GOSUB/THEN/ELSE/RESTORE, ON..
 * GOTO). These turn those numbers into navigation: jump to a line, follow the
 * reference under the cursor (with a back stack), and flag jumps to lines that
 * do not exist. They reuse the same reference grammar the auto-renumber uses. */

/* The line whose BASIC number == n, or 0. */
static Line *line_with_num(int n)
{
    Line *p;
    if (n < 0) return 0;
    for (p = first; p; p = p->next) if (line_num(p) == n) return p;
    return 0;
}

/* Move the cursor to the start of line p's code and show it. */
static void goto_line_ptr(Line *p)
{
    cur = p; lineno = line_index(p) + 1;
    curx = body_off_of(p); coloff = 0;
    sel_anchor = 0;
    scroll_into_view();
}

/* The decimal number the cursor sits on (or just past), or -1. */
static int num_under_cursor(void)
{
    int i = curx, n = 0;
    if (i > cur->len) i = cur->len;
    if (!(i < cur->len && is_digit(cur->s[i])) && i > 0 && is_digit(cur->s[i - 1])) i--;
    if (!(i < cur->len && is_digit(cur->s[i]))) return -1;
    while (i > 0 && is_digit(cur->s[i - 1])) i--;
    while (i < cur->len && is_digit(cur->s[i])) n = n * 10 + (cur->s[i++] - '0');
    return n;
}

/* Back stack of line indices left by Follow/Go to Line. */
#define JMP_MAX 32
static int jmp_stk[JMP_MAX], jmp_sp;
static void jmp_push(void) { if (jmp_sp < JMP_MAX) jmp_stk[jmp_sp++] = line_index(cur); }
static void nav_reset(void) { jmp_sp = 0; }

/* message "Line N does not exist." */
static void msg_no_line(int n)
{
    char m[32]; int k = 0; const char *p;
    for (p = "Line "; *p; p++) m[k++] = *p;
    k += fmt_uint(m + k, n);
    for (p = " does not exist."; *p; p++) m[k++] = *p;
    m[k] = 0; message(m);
}

/* F4 : jump to the line named by the number under the cursor. */
static void do_follow_ref(void)
{
    int n; Line *t;
    if (!is_basic) { message("Reference jumps work with BASIC (.BAS) files."); return; }
    n = num_under_cursor();
    if (n < 0) { message("Put the cursor on a line number to follow it."); return; }
    t = line_with_num(n);
    if (!t) { msg_no_line(n); return; }
    jmp_push();
    goto_line_ptr(t);
}

/* F8 : return to where the last Follow/Go to Line jumped from. */
static void do_jump_back(void)
{
    if (jmp_sp == 0) { message("Nowhere to jump back to."); return; }
    goto_line_ptr(line_at(jmp_stk[--jmp_sp]));
}

/* Ctrl-G : prompt for a line number and jump to it. */
static void do_goto_line(void)
{
    char buf[16]; int n = 0; const char *q; Line *t;
    if (!is_basic) { message("Go to line works with BASIC (.BAS) files."); return; }
    if (!prompt("Go to line:", buf, sizeof buf)) return;
    for (q = buf; *q; q++) if (*q >= '0' && *q <= '9') n = n * 10 + (*q - '0');
    t = line_with_num(n);
    if (!t) { msg_no_line(n); return; }
    jmp_push();
    goto_line_ptr(t);
}

/* Scan every GOTO/GOSUB/THEN/ELSE/RESTORE (and ON..GOTO list) target; report any
 * that name a line which does not exist, and jump to the first one. */
static void do_check_refs(void)
{
    Line *p, *bad = 0; int badnum = 0, badcol = 0, count = 0;
    if (!is_basic) { message("Reference checking works with BASIC (.BAS) files."); return; }
    for (p = first; p; p = p->next) {
        const char *s = p->s; int len = p->len, i = num_prefix_len(p);
        while (i < len) {
            if (s[i] == '"') { i++; while (i < len && s[i] != '"') i++; if (i < len) i++; continue; }
            if (s[i] == '\'') break;                         /* rest of line is a comment */
            if (is_alpha(s[i])) {
                int j = i;
                while (j < len && (is_alpha(s[j]) || is_digit(s[j]))) j++;
                if (is_ref_kw(s + i, j - i)) {
                    i = j;
                    for (;;) {                               /* comma-list of line numbers */
                        while (i < len && s[i] == ' ') i++;
                        if (i < len && is_digit(s[i])) {
                            int r = 0, col = i;
                            while (i < len && is_digit(s[i])) r = r * 10 + (s[i++] - '0');
                            if (!line_with_num(r)) {
                                count++;
                                if (!bad) { bad = p; badnum = r; badcol = col; }
                            }
                            while (i < len && s[i] == ' ') i++;
                            if (i < len && s[i] == ',') { i++; continue; }
                        }
                        break;
                    }
                } else i = j;
            } else if (is_digit(s[i])) { while (i < len && is_digit(s[i])) i++; }
            else i++;
        }
    }
    if (count == 0) { message("All line references are valid."); return; }
    cur = bad; curx = badcol; lineno = line_index(cur) + 1; coloff = 0; sel_anchor = 0;
    scroll_into_view();
    { char m[64]; int k = 0; const char *q;
      for (q = "Broken reference to line "; *q; q++) m[k++] = *q;
      k += fmt_uint(m + k, badnum);
      if (count > 1) {
          for (q = " (+"; *q; q++) m[k++] = *q;
          k += fmt_uint(m + k, count - 1);
          for (q = " more)"; *q; q++) m[k++] = *q;
      }
      m[k++] = '.'; m[k] = 0; message(m); }
}

/* ------------------------------------------------- PROC/FN OUTLINE */
/* A quick index of the program's procedures and functions: every `DEF PROCname`
 * / `DEF FNname` (glued BBC form) and `DEF proc NAME` / `DEF fn NAME` (spaced
 * form). Pick one from a list to jump to its definition. */

#define OUTLINE_MAX 128
static char outline_lbl[OUTLINE_MAX][40];   /* "<line>  PROCname(args)" */
static int  outline_ln[OUTLINE_MAX];        /* line index of each DEF   */
static int  outline_n;

/* Is s[i..] the standalone keyword "DEF" (case-insensitive) followed by space? */
static int is_def_kw(const char *s, int i, int len)
{
    return len - i >= 4 && upc(s[i]) == 'D' && upc(s[i+1]) == 'E' && upc(s[i+2]) == 'F'
           && s[i+3] == ' ';
}

/* True if the body at s[i..] starts with PROC or FN (either case = a PROC/FN). */
static int is_procfn(const char *s, int i, int len)
{
    if (len - i >= 4 && upc(s[i])=='P' && upc(s[i+1])=='R' && upc(s[i+2])=='O' && upc(s[i+3])=='C')
        return 1;
    if (len - i >= 2 && upc(s[i])=='F' && upc(s[i+1])=='N')
        return 1;
    return 0;
}

/* Collect every DEF PROC/FN into the outline_* arrays. */
static void outline_scan(void)
{
    Line *p; int idx = 0;
    outline_n = 0;
    for (p = first; p; p = p->next, idx++) {
        const char *s = p->s; int len = p->len, i = num_prefix_len(p);
        int k, num;
        while (i < len && s[i] == ' ') i++;
        if (!is_def_kw(s, i, len)) continue;
        i += 3; while (i < len && s[i] == ' ') i++;    /* past "DEF " */
        if (!is_procfn(s, i, len)) continue;
        if (outline_n >= OUTLINE_MAX) break;
        num = line_num(p);
        k = 0;
        if (num >= 0) { k = fmt_uint(outline_lbl[outline_n], num);
                        outline_lbl[outline_n][k++] = ' '; outline_lbl[outline_n][k++] = ' '; }
        while (i < len && k < (int)sizeof outline_lbl[0] - 1) outline_lbl[outline_n][k++] = s[i++];
        while (k > 0 && outline_lbl[outline_n][k-1] == ' ') k--;
        outline_lbl[outline_n][k] = 0;
        outline_ln[outline_n] = idx;
        outline_n++;
    }
}

/* Show the collected procedures in a scrollable box; return the chosen index or
 * -1 on cancel. */
static int outline_pick(void)
{
    int i, sel = 0, top = 0, w = 16, x, y, h;
    int maxrows = ROWS - 8, rows;
    if (maxrows < 3) maxrows = 3;
    rows = outline_n < maxrows ? outline_n : maxrows;
    for (i = 0; i < outline_n; i++) { int l = (int)strlen(outline_lbl[i]) + 3; if (l > w) w = l; }
    if (w > COLS - 4) w = COLS - 4;
    h = rows + 2;
    x = (COLS - w) / 2; y = (ROWS - h) / 2;

    redraw();
    for (;;) {
        int c;
        draw_box(x, y, w, h, "Procedures");
        for (i = 0; i < rows; i++) {
            int idx = top + i;
            bg(idx == sel ? C_CYAN : C_WHITE); fg(C_BLACK);
            repeat_ch(x + 1, y + 1 + i, ' ', w - 2);
            if (idx < outline_n) puts_at(x + 2, y + 1 + i, outline_lbl[idx]);
        }
        c = key();
        if (c == K_ESC)   { redraw(); return -1; }
        if (c == K_ENTER) { redraw(); return sel; }
        if (c == K_UP)    { if (sel > 0) sel--; }
        if (c == K_DOWN)  { if (sel < outline_n - 1) sel++; }
        if (c == K_PGUP)  { sel -= rows; if (sel < 0) sel = 0; }
        if (c == K_PGDN)  { sel += rows; if (sel >= outline_n) sel = outline_n - 1; }
        if (c == K_HOME)  sel = 0;
        if (c == K_END)   sel = outline_n - 1;
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
    }
}

/* F9 : list the procedures/functions and jump to the chosen one. */
static void do_outline(void)
{
    int sel;
    if (!is_basic) { message("The outline is for BASIC (.BAS) files."); return; }
    outline_scan();
    if (outline_n == 0) { message("No PROC/FN definitions found."); return; }
    sel = outline_pick();
    if (sel < 0) return;
    jmp_push();
    goto_line_ptr(line_at(outline_ln[sel]));
}

/* Run the current BASIC program: save it, run it synchronously on the visible
 * screen, then repaint the editor unchanged. The editor never exits, so the
 * cursor, scroll, selection and text are all exactly as they were. Always
 * returns 0 (stay in the editor). */
static int run_current(void)
{
    static const char *pause = "\n[ press a key to return to the editor ]\n";
    int ci, ti, si, scurx, scoloff, sselc;

    if (!is_basic) { message("Run works with BASIC (.BAS) files."); return 0; }
    if (!strcmp(filename, "NONAME.TXT")) {
        do_saveas();
        if (!strcmp(filename, "NONAME.TXT")) return 0;      /* Save As cancelled: stay */
    } else if (save_file(filename) < 0) {
        message("Could not save; not run.");
        return 0;
    }
    changed = 0;
    if (!S->run_basic) { message("Run is unavailable."); return 0; }

    /* The run resets the seed heap (a fresh slate for the program) - which is the
     * heap our line list is malloc'd from. So remember the cursor/scroll as
     * indices, run, then rebuild the list from the (just-saved) file on the clean
     * heap and restore the position. Content is identical since we just saved. */
    ci = line_index(cur);
    ti = line_index(top);
    si = sel_anchor ? line_index(sel_anchor) : -1;
    scurx = curx; scoloff = coloff; sselc = sel_acol;

    S->run_basic(filename);                 /* blocks on the visible screen until it stops */

    S->screen_save();                       /* stash the output so Alt+F5 can show it later */
    S->puts(pause, (int)strlen(pause));     /* let the user read the output */
    S->getkey();                            /* wait for any key */

    first = 0;                              /* abandon the dead list (its heap was reset) */
    undo_forget(); nav_reset();             /* undo snapshots lived in that heap too */
    S->gfx_backbuffer(1);                   /* re-enable the editor's double buffer */
    if (load_file(filename) < 0) {          /* rebuild fresh from disk */
        S->puts("ed: out of memory\n", 18);
        return 1;                           /* fatal: leave the editor */
    }
    cur = line_at(ci);
    top = line_at(ti);
    sel_anchor = (si >= 0) ? line_at(si) : 0;
    sel_acol = sselc;
    curx = scurx; coloff = scoloff;
    lineno = ci + 1;
    if (curx > cur->len) curx = cur->len;
    if (curx < body_off()) curx = body_off();

    bg(C_BLUE); fg(C_WHITE); cls();
    scroll_into_view();
    redraw();
    return 0;                               /* stay in the editor */
}

/* ------------------------------------------------------------ DEBUGGER
 * The editor is a CAP_DEBUG front-end: it collects gutter breakpoints, then
 * Debug arms them in the interpreter and runs the program, pausing into the
 * on_stop overlay below. Because RUN wipes the seed heap our line list lives
 * in, the overlay draws the source from the interpreter (dbg_line_at), not from
 * our own (now-dead) buffer - the same discipline run_current already follows. */

static void toggle_break(void)
{
    int ln;
    if (!is_basic) { message("Breakpoints work with BASIC (.BAS) files."); return; }
    ln = line_num(cur);
    if (ln < 0) { message("This line has no line number to break on."); return; }
    for (int i = 0; i < brk_n; i++)
        if (brk_lines[i] == ln) {                       /* already set: toggle it off */
            for (int j = i; j < brk_n - 1; j++) brk_lines[j] = brk_lines[j + 1];
            brk_n--; return;
        }
    if (brk_n < MAX_BREAKS) brk_lines[brk_n++] = ln;
    else message("Too many breakpoints.");
}
static void clear_breaks(void) { brk_n = 0; message("All breakpoints cleared."); }

static int dbg_view;    /* the overlay's cursor line (index into the program table) */

/* Small unsigned -> decimal helper (fmt_uint is defined later; keep local). */
static void dbg_num(int v, char *out)
{
    char t[12]; int n = 0;
    if (v < 0) { *out++ = '-'; v = -v; }
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    int i = 0; while (n) out[i++] = t[--n]; out[i] = 0;
}

static const char *dbg_event_name(int ev)
{
    switch (ev) {
        case BERRY_DBG_ERROR:   return "error";
        case BERRY_DBG_CALL:    return "call";
        case BERRY_DBG_WRITE:   return "write";
        case BERRY_DBG_KEYWORD: return "keyword";
        default:                return "paused";
    }
}

/* Format one variable's value the way the panel shows it. */
static void dbg_fmt_val(const char *name, int isstr, char *out, int outsz)
{
    if (isstr) {
        char b[40]; int n = S->get_str(name, b, sizeof b - 1);
        if (n < 0) n = 0; if (n > (int)sizeof b - 1) n = sizeof b - 1; b[n] = 0;
        out[0] = '"'; strncpy(out + 1, b, outsz - 3);
        int L = (int)strlen(out); out[L] = '"'; out[L + 1] = 0;
    } else {
        double v = 0; S->get_num(name, &v);
        int n = S->fmt_num(v, out);      /* fmt_num returns the length, no NUL */
        if (n < 0) n = 0; if (n > outsz - 1) n = outsz - 1;
        out[n] = 0;
    }
}

/* The debugger view, drawn INSIDE the editor's own chrome so it feels like the
 * editor paused, not a separate program: the real menu bar, the blue framed
 * window with the filename in its title, the gutter (cyan line numbers + red
 * breakpoint dots) and syntax highlighting exactly as when editing, a docked
 * Watch panel on the right, and the editor's status bar showing debug controls.
 * The source is read from the interpreter (dbg_line_at) because RUN wiped our
 * own line buffer - but it is the same program we just saved, drawn the same. */
#define DBG_PANEL_W 26

static void dbg_draw(const dbg_ctx *c)
{
    int total  = S->dbg_line_count();
    int cur_idx = c->line_idx;
    int panelw = (COLS >= 64) ? DBG_PANEL_W : 0;         /* too narrow -> no panel */
    int dvx    = panelw ? (win_x2 - panelw) : (win_x2 + 1);
    int codeR  = panelw ? (dvx - 1) : win_x2;            /* rightmost code column */
    int nrows  = win_y2 - win_y1 + 1;
    unsigned char clsbuf[MAXLEN + 1];

    /* gutter width from the widest line number: 1 marker col + digits + 1 space,
     * matching the editor's own gutter formula. */
    int maxnum = 0;
    for (int i = 0; i < total; i++) { int nn = 0; char t[8];
        if (S->dbg_line_at(i, &nn, t, sizeof t) == 0 && nn > maxnum) maxnum = nn; }
    int nd = 1; { int t = maxnum; while (t >= 10) { t /= 10; nd++; } }
    int gw = nd + 2;

    /* scroll so the current line (and the nav cursor) stay visible */
    if (dbg_view < 0 || dbg_view >= total) dbg_view = cur_idx;
    int topi = dbg_view - nrows / 2;
    if (cur_idx < topi)          topi = cur_idx;
    if (cur_idx >= topi + nrows) topi = cur_idx - nrows + 1;
    if (topi > total - nrows)    topi = total - nrows;
    if (topi < 0)                topi = 0;

    /* editor chrome */
    bg(C_BLUE); cls();
    draw_menubar();
    draw_frame();
    if (panelw) { bgrgb(pal[C_BLUE]); fg(C_WHITE);
                  at(dvx, win_y1 - 1); out(203); at(dvx, win_y2 + 1); out(202); }  /* T-junctions */

    /* --- code area --- */
    for (int r = 0; r < nrows; r++) {
        int row = win_y1 + r, idx = topi + r, number = 0; char text[128];
        int is_cur = (idx == cur_idx), is_sel = (idx == dbg_view);
        int ok = (idx >= 0 && idx < total && S->dbg_line_at(idx, &number, text, sizeof text) == 0);

        /* gutter: darker blue, right-aligned number, marker/arrow in the left cell */
        bgrgb(0x0000A0);
        at(win_x1, row);
        if (ok) {
            int ndi = 1; { int t = number; while (t >= 10) { t /= 10; ndi++; } }
            int pad = gw - 1 - ndi; if (pad < 0) pad = 0;
            char ns[12]; dbg_num(number, ns);
            fgrgb(syn_color(HL_LINENO));
            for (int i = 0; i < pad; i++) out(' ');
            for (int k = 0; ns[k]; k++) out((unsigned char)ns[k]);
            out(' ');
            if (has_brk(number)) { fgrgb(0xFF4040); at(win_x1, row); out('*'); }
            else if (is_cur)     { fg(C_BLACK);     at(win_x1, row); out(16);  }  /* filled arrow */
            else if (is_sel)     { fgrgb(0xFFE060); at(win_x1, row); out(175); }  /* hollow arrow */
        } else {
            for (int i = 0; i < gw; i++) out(' ');
        }

        /* body: highlight the current line (cyan, like a selection) and the nav
         * cursor (steel-blue); everything else is syntax-coloured on window blue. */
        unsigned rbg = is_cur ? pal[C_CYAN] : (is_sel ? 0x335A8C : pal[C_BLUE]);
        if (ok) hl_basic(text, (int)strlen(text), clsbuf);
        at(win_x1 + gw, row);
        for (int col = win_x1 + gw; col <= codeR; col++) {
            int i = col - (win_x1 + gw);
            bgrgb(rbg);
            if (ok && i < (int)strlen(text)) {
                fgrgb(is_cur ? pal[C_BLACK] : syn_color(clsbuf[i]));
                out((unsigned char)text[i]);
            } else out(' ');
        }
        if (panelw) { bgrgb(pal[C_BLUE]); fg(C_WHITE); at(dvx, row); out(B_V); }
    }

    /* --- Watch panel (variables + call stack), styled like the editor --- */
    if (panelw) {
        int col = dvx + 1, row = win_y1;
        bgrgb(pal[C_BLUE]);
        for (int rr = win_y1; rr <= win_y2; rr++) repeat_ch(col, rr, ' ', win_x2 - col + 1);
        fgrgb(0xFFE060); puts_at(col, row++, "VARIABLES");
        int nv = S->dbg_var_count(), half = nrows / 2;
        for (int i = 0; i < nv && row < win_y1 + half; i++) {
            char name[16]; int isstr = 0, isarr = 0;
            if (S->dbg_var_at(i, name, sizeof name, &isstr, &isarr) < 0) continue;
            char val[40], line[52];
            if (isarr) strcpy(val, "(array)"); else dbg_fmt_val(name, isstr, val, sizeof val);
            strcpy(line, name); strcat(line, "="); strncat(line, val, 36);
            line[win_x2 - col] = 0;
            fgrgb(0xE0E0E0); puts_at(col, row++, line);
        }
        row++;
        if (row <= win_y2) { fgrgb(0xFFE060); puts_at(col, row++, "CALL STACK"); }
        int depth = S->dbg_stack_depth();
        if (depth == 0 && row <= win_y2) { fgrgb(0x80FF80); puts_at(col, row++, " (top level)"); }
        for (int i = 0; i < depth && row <= win_y2; i++) {
            char name[16], ns[12], line[40]; int cl = 0;
            if (S->dbg_stack_frame(i, name, sizeof name, &cl) < 0) break;
            strcpy(line, " "); strcat(line, name); strcat(line, " @"); dbg_num(cl, ns); strcat(line, ns);
            fgrgb(0x80FF80); puts_at(col, row++, line);
        }
    }

    /* --- status bar: the editor's white bar, debug content --- */
    { char b[96], ns[12], ev[16];
      bg(C_WHITE); fg(C_BLACK); repeat_ch(0, ROWS - 1, ' ', COLS);
      { const char *e = dbg_event_name(c->event); int i = 0;
        for (; e[i] && i < 15; i++) ev[i] = (e[i] >= 'a' && e[i] <= 'z') ? e[i] - 32 : e[i]; ev[i] = 0; }
      strcpy(b, "  "); strcat(b, ev);
      strcat(b, "  line "); dbg_num(c->line, ns); strcat(b, ns);
      { int sn = 0; char st[8]; if (S->dbg_line_at(dbg_view, &sn, st, sizeof st) == 0 && sn != c->line) {
            strcat(b, "   sel "); dbg_num(sn, ns); strcat(b, ns); } }
      if (c->event == BERRY_DBG_ERROR && c->errmsg) { strcat(b, ": "); strncat(b, c->errmsg, 30); }
      puts_at(0, ROWS - 1, b);
      const char *keys = " F7 Step  F8 Over  F6 Out  F5 Cont  F4 Run  F9 Bp  F10 Menu  Esc Stop ";
      int kl = (int)strlen(keys);
      if (COLS > kl + 4) puts_at(COLS - kl, ROWS - 1, keys);
    }
}

/* Toggle a breakpoint on the nav-cursor line (F9 and the Debug menu). */
static void dbg_bp_toggle_view(void)
{
    int number = 0; char t[8];
    if (S->dbg_line_at(dbg_view, &number, t, sizeof t) != 0) return;
    if (has_brk(number)) {
        S->dbg_clear(number);
        for (int i = 0; i < brk_n; i++) if (brk_lines[i] == number) {
            for (int j = i; j < brk_n - 1; j++) brk_lines[j] = brk_lines[j + 1]; brk_n--; break; }
    } else if (brk_n < MAX_BREAKS) {
        S->dbg_break_line(number); brk_lines[brk_n++] = number;
    }
}

/* Run to the nav-cursor line: a breakpoint there, then continue. Returns 1 so
 * the caller leaves the stop loop and resumes the program. */
static int dbg_run_to_view(void)
{
    int number = 0; char t[8];
    if (S->dbg_line_at(dbg_view, &number, t, sizeof t) != 0) return 0;
    S->dbg_break_line(number);
    S->dbg_cont();
    return 1;
}

/* The in-editor Debug menu (F10 while paused), using the editor's own dropdown. */
static const char *dbg_menu_items[] = {
    " Step Into     F7 ", " Step Over     F8 ", " Step Out      F6 ",
    " Continue      F5 ", "-", " Run to Line   F4 ", " Toggle Break  F9 ",
    "-", " Stop         Esc ", 0
};

/* Show the running/finished program's output - its console text AND graphics,
 * mode 1 or 2 - which was stashed with screen_save. Alt+F5 (or Esc) returns. The
 * output is drawn to the visible front directly (buffering off) so the editor's
 * double buffer never overwrites it; the caller redraws its own UI afterwards. */
static void show_output(void)
{
    int wasbuf = S->gfx_buffered();
    S->gfx_backbuffer(0);                          /* draw straight to the front */
    for (;;) {
        S->screen_restore();                       /* the saved program screen */
        bg(C_WHITE); fg(C_BLACK); repeat_ch(0, ROWS - 1, ' ', COLS);
        puts_at(0, ROWS - 1, "  PROGRAM OUTPUT      Alt+F5 or Esc: back to the editor");
        int k = key();                             /* gfx_flip is a no-op while unbuffered */
        int m = S->keymods();
        if (k == K_ESC || ((m & KMOD_ALT) && k == K_F5)) break;
    }
    if (wasbuf) S->gfx_backbuffer(1);              /* restore the caller's buffering */
}

/* The interactive stop handler: draws the paused editor and drives stepping.
 * On entry the visible screen holds the program's output so far, so stash it
 * (Alt+F5 shows it again) and force buffering off so the debug UI draws to the
 * front; restore the program's buffering when we resume. */
static void ed_on_stop(const dbg_ctx *c)
{
    int wasbuf = S->gfx_buffered();
    if (wasbuf) S->gfx_backbuffer(0);
    S->screen_save();                      /* the program's output up to this pause */
    dbg_view = c->line_idx;

    int done = 0;
    while (!done) {
        dbg_draw(c);
        int k = key();
        int mods = S->keymods();
        int nrows = win_y2 - win_y1 + 1;
        int last = S->dbg_line_count() - 1;
        if ((mods & KMOD_ALT) && k == K_F5) { show_output(); continue; }  /* peek at output */
        switch (k) {
            case K_F7: case 's':               S->dbg_step();      done = 1; break;
            case K_F8: case 'o':               S->dbg_step_over(); done = 1; break;
            case K_F6: case 'u':               S->dbg_step_out();  done = 1; break;
            case K_F5: case 'c': case K_ENTER: S->dbg_cont();      done = 1; break;
            case K_F4: case 'g':               if (dbg_run_to_view()) done = 1; break;
            case 'q': case K_ESC:              S->dbg_abort();     done = 1; break;
            case K_UP:   if (dbg_view > 0) dbg_view--; break;
            case K_DOWN: if (dbg_view < last) dbg_view++; break;
            case K_PGUP: dbg_view -= nrows; if (dbg_view < 0) dbg_view = 0; break;
            case K_PGDN: dbg_view += nrows; if (dbg_view > last) dbg_view = last; break;
            case K_HOME: dbg_view = 0; break;
            case K_END:  dbg_view = last; break;
            case K_F9:   dbg_bp_toggle_view(); break;
            case K_F10: {                       /* the editor's dropdown, Debug menu */
                int n = 0; while (dbg_menu_items[n]) n++;
                int sel = popup(2, 1, dbg_menu_items, n);
                switch (sel) {
                    case 0: S->dbg_step();      done = 1; break;
                    case 1: S->dbg_step_over(); done = 1; break;
                    case 2: S->dbg_step_out();  done = 1; break;
                    case 3: S->dbg_cont();      done = 1; break;
                    case 5: if (dbg_run_to_view()) done = 1; break;
                    case 6: dbg_bp_toggle_view(); break;
                    case 8: S->dbg_abort();     done = 1; break;
                    default: break;             /* cancelled / separators */
                }
                break;
            }
            default: break;
        }
    }
    /* Put the program's own screen back before it resumes, so its console/graphics
     * output continues coherently over what it drew - not over our debug UI. */
    S->screen_restore();
    if (wasbuf) S->gfx_backbuffer(1);       /* hand the program back its buffering */
}

/* Debug: like Run, but arm the debugger with the editor's breakpoints first. */
static int run_debug(void)
{
    int ci, ti, si, scurx, scoloff, sselc;

    if (!is_basic) { message("Debug works with BASIC (.BAS) files."); return 0; }
    if (!S->dbg_run || !S->dbg_attach) { message("The debugger is unavailable."); return 0; }
    if (!strcmp(filename, "NONAME.TXT")) {
        do_saveas();
        if (!strcmp(filename, "NONAME.TXT")) return 0;
    } else if (save_file(filename) < 0) {
        message("Could not save; not debugged.");
        return 0;
    }
    changed = 0;

    ci = line_index(cur); ti = line_index(top);
    si = sel_anchor ? line_index(sel_anchor) : -1;
    scurx = curx; scoloff = coloff; sselc = sel_acol;

    S->dbg_attach(ed_on_stop);
    S->dbg_break_error(1);                  /* also drop in on an uncaught error */
    if (brk_n == 0) S->dbg_break_every(1);  /* no breakpoints set: single-step from the top */
    else { S->dbg_clear(0); for (int i = 0; i < brk_n; i++) S->dbg_break_line(brk_lines[i]); }

    dbg_view = -1;
    S->dbg_run(filename);                   /* runs on the visible screen, calling ed_on_stop */
    S->dbg_detach();

    S->screen_save();                       /* stash the final output for Alt+F5 */
    { static const char *pause = "\n[ press a key to return to the editor ]\n";
      S->puts(pause, (int)strlen(pause)); S->getkey(); }

    first = 0;                              /* the list's heap was reset by the run */
    undo_forget(); nav_reset();
    S->gfx_backbuffer(1);
    if (load_file(filename) < 0) { S->puts("ed: out of memory\n", 18); return 1; }
    cur = line_at(ci); top = line_at(ti);
    sel_anchor = (si >= 0) ? line_at(si) : 0; sel_acol = sselc;
    curx = scurx; coloff = scoloff; lineno = ci + 1;
    if (curx > cur->len) curx = cur->len;
    if (curx < body_off()) curx = body_off();
    bg(C_BLUE); fg(C_WHITE); cls();
    scroll_into_view(); redraw();
    return 0;
}

/* Delete the current line (also Ctrl-Y). Never empties the list. */
static void delete_line(void)
{
    undo_record();
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
    if (menu == 0) {                                  /* File */
        switch (item) {
        case 0: do_new();    break;
        case 1: do_open();   break;
        case 2: do_save();   break;
        case 3: do_saveas(); break;
        case 4: return 1;                             /* Quit */
        }
    } else if (menu == 1) {                           /* Edit (items 2, 6, 10 are separators) */
        switch (item) {
        case 0: do_undo();      break;
        case 1: do_redo();      break;
        case 3: if (sel_anchor) { undo_record(); sel_copy(); sel_delete(); } break;  /* Cut */
        case 4: sel_copy();     break;                               /* Copy  */
        case 5: do_paste();     break;                               /* Paste */
        case 7: delete_line();  break;
        case 8: goto_top();     break;
        case 9: goto_bottom();  break;
        case 11: do_find();     break;
        case 12: find_next();   break;
        case 13: do_replace();  break;
        }
    } else if (menu == BASIC_MENU) {                  /* Basic (BASIC files only) */
        switch (item) {
        case 0: return run_current();                 /* Run           */
        case 1: return run_debug();                   /* Debug         */
        case 2: toggle_break();   break;              /* Toggle Break  */
        case 3: clear_breaks();   break;              /* Clear Breaks  */
        case 5: toggle_autonum(); break;              /* Auto Number (4,6 separators) */
        case 7: do_outline();     break;
        case 8: do_goto_line();   break;
        case 9: do_follow_ref();  break;
        case 10: do_jump_back();  break;
        case 11: do_check_refs(); break;
        }
    } else {                                          /* Help */
        switch (item) {
        case 0: message("ed - a full-screen text editor for BerryBasiC"); break;
        }
    }
    return 0;
}

/* Which menu an Alt+letter accelerator opens (Alt+F/E/H), matching each menu
 * title's first letter, case-insensitive. -1 if the letter names no menu. */
static int menu_accel(int c)
{
    int up = (c >= 'a' && c <= 'z') ? c - 32 : c;
    for (int i = 0; i < NMENU_ALL; i++) {
        if (!menu_visible(i)) continue;
        const char *t = menus[i].title;
        while (*t == ' ') t++;                        /* first letter of the title */
        int tu = (*t >= 'a' && *t <= 'z') ? *t - 32 : *t;
        if (tu == up) return i;
    }
    return -1;
}

/* Drive the menu bar starting on menu `active` (F10 opens File; Alt+letter opens
 * that menu). Left/Right move between File/Edit/Help, Down/Enter pick an item,
 * Esc leaves. Returns 1 if the editor should quit. */
static int run_menu_at(int active)
{
    int quit = 0;

    if (!menu_visible(active)) active = menu_step(active, 1);   /* defensive */
    for (;;) {
        redraw();                        /* erase any previous dropdown first */
        draw_bar(active);
        int sel = popup(menu_col(active), 1, menus[active].items,
                        menu_count(menus[active].items));
        if (sel == NAV_LEFT)  { active = menu_step(active, -1); continue; }
        if (sel == NAV_RIGHT) { active = menu_step(active,  1); continue; }
        if (sel == NAV_CANCEL) break;                 /* Esc: leave the menu */
        quit = menu_action(active, sel);
        break;
    }
    redraw();
    return quit;
}

static int run_menu(void) { return run_menu_at(0); }

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
        "  Shift + move keys        select text",
        "  Ctrl-C / Ctrl-Ins        copy       (to the system clipboard)",
        "  Ctrl-X / Shift-Del       cut",
        "  Ctrl-V / Shift-Ins       paste",
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

/* Size the editor to the real screen: the console font cell (con_font) and the
 * framebuffer size (gfx_width/height) give the character grid in cells. Falls
 * back to 8x16 / 80x25 when the values are unavailable (e.g. the host build). */
static void layout(void)
{
    int cw = 0, ch = 0, pw, ph;
    S->con_font(&cw, &ch);
    if (cw > 0) CELLW = cw;
    if (ch > 0) CELLH = ch;

    pw = S->gfx_width();
    ph = S->gfx_height();
    if (pw > 0 && CELLW > 0) COLS = pw / CELLW;
    if (ph > 0 && CELLH > 0) ROWS = ph / CELLH;
    if (COLS < 20) COLS = 80;
    if (ROWS < 8)  ROWS = 25;

    win_x1 = 1;                         /* left border hugs column 0 */
    win_y1 = 2;
    win_x2 = COLS - 2;                  /* right border hugs the last column */
    win_y2 = ROWS - 4;                  /* leave the status bar (ROWS-1) + border */

    wrapcol = win_x2 - win_x1;          /* wrap at the window's right edge */
    if (wrapcol > MAXLEN - 1) wrapcol = MAXLEN - 1;
    if (wrapcol < 8)          wrapcol = 0;   /* too narrow: don't wrap */

    is_basic = name_is_basic(filename); /* so the first scroll/paint sees the right width */
    update_gutter();
}

int main(int argc, char **argv)
{
    int c, i, quit = 0;

    S = berry_svc;                        /* crt0 stashed the services table here */
    layout();                           /* adapt to the real screen size */
    S->gfx_backbuffer(1);               /* compose every frame off-screen */

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

    is_basic = name_is_basic(filename);
    if (is_basic && auto_num) autonum_ensure();     /* every line gets a managed number */
    if (curx < body_off()) curx = body_off();

    bg(C_BLUE); fg(C_WHITE);
    cls();
    scroll_into_view();                 /* set top/cury so the caret starts in the text */
    redraw();

    while (!quit) {
        c = key();
        if (curx < body_off()) curx = body_off();   /* edits/moves act on code, never the number */

        int mods = S->keymods(), handled = 0;

        /* Alt+letter opens a menu by its first letter (Alt+F/E/R/H), and Alt+N
         * toggles auto-numbering (Ctrl+N is left for the usual "New"). Left Alt
         * only, so AltGr (third-legend characters) still types normally. */
        if (mods & KMOD_ALT) {
            if (c == K_F5) show_output();        /* Alt+F5: view the last program output */
            else if (c == 'n' || c == 'N') toggle_autonum();
            else { int mi = menu_accel(c); if (mi >= 0) quit = run_menu_at(mi); }
            handled = 1;
        }
        /* System clipboard. USB gives the plain key + a modifier bit; a serial
         * terminal sends the raw control code (0x03/0x16/0x18). */
        else if (((mods & KMOD_CTRL) && (c == 'c' || c == 'C')) || ((mods & KMOD_CTRL) && c == K_INS) || c == 0x03) {
            sel_copy(); handled = 1;                                  /* copy  */
        }
        else if (((mods & KMOD_CTRL) && (c == 'x' || c == 'X')) || ((mods & KMOD_SHIFT) && c == K_DEL) || c == 0x18) {
            if (sel_anchor) { undo_record(); sel_copy(); sel_delete(); } handled = 1;  /* cut */
        }
        else if (((mods & KMOD_CTRL) && (c == 'v' || c == 'V')) || ((mods & KMOD_SHIFT) && c == K_INS)) {
            do_paste(); handled = 1;                                  /* paste (NB: no raw 0x16
                                     * fallback - that code IS K_END, which it was swallowing) */
        }
        else if ((mods & KMOD_CTRL) && (c == 'z' || c == 'Z')) {      /* Ctrl-Z undo / Ctrl-Shift-Z redo */
            if (mods & KMOD_SHIFT) do_redo(); else do_undo(); handled = 1;
        }
        else if ((mods & KMOD_CTRL) && (c == 'y' || c == 'Y')) { delete_line(); handled = 1; }  /* Ctrl-Y */
        else if ((mods & KMOD_CTRL) && (c == 'f' || c == 'F')) { do_find();    handled = 1; }  /* Ctrl-F */
        else if ((mods & KMOD_CTRL) && (c == 'r' || c == 'R')) { do_replace(); handled = 1; }  /* Ctrl-R */
        else if ((mods & KMOD_CTRL) && (c == 'g' || c == 'G')) { do_goto_line(); handled = 1; }  /* Ctrl-G */
        else if ((mods & KMOD_CTRL) && (c == 'p' || c == 'P')) { do_outline();   handled = 1; }  /* Ctrl-P */
        else if ((mods & KMOD_CTRL) && (c == 'd' || c == 'D')) { quit = run_debug();   handled = 1; }  /* Ctrl-D debug */
        else if ((mods & KMOD_CTRL) && (c == 'b' || c == 'B')) { toggle_break();       handled = 1; }  /* Ctrl-B breakpoint */
        else if ((mods & KMOD_CTRL) && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) { handled = 1; }  /* swallow */

        if (!handled) {
            /* Shift + a movement key extends the selection; any other movement
             * clears it; an edit over a live selection replaces it. */
            int is_move = (c == K_LEFT || c == K_RIGHT || c == K_UP || c == K_DOWN ||
                           c == K_HOME || c == K_END || c == K_PGUP || c == K_PGDN);
            if (is_move) {
                if (mods & KMOD_SHIFT) { if (!sel_anchor) { sel_anchor = cur; sel_acol = curx; } }
                else sel_anchor = 0;
            } else if (sel_anchor && (c == K_BS || c == K_DEL)) {
                undo_record(); sel_delete(); handled = 1;  /* delete selection = the edit */
            } else if (sel_anchor && (c == K_ENTER || c == K_TAB || (c >= 32 && c < 256))) {
                undo_record(); undo_group = 1;             /* one undo step for delete+insert */
                sel_delete();                              /* replace: delete then insert */
            }
        }

        if (!handled) switch (c) {
        case K_LEFT:
            if (curx > body_off()) curx--;                 /* stop at the code, not the number */
            else if (cur->prev) { go_up(); curx = cur->len; }
            break;
        case K_RIGHT:
            if (curx < cur->len) curx++;
            else if (cur->next) { go_down(); curx = body_off(); }
            break;
        case K_UP:    go_up();   break;
        case K_DOWN:  go_down(); break;
        case K_HOME:  curx = body_off(); coloff = 0; break;
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
            do_save();
            break;

        case K_F5:                      /* run the current BASIC program */
            quit = run_current();
            break;

        case K_F4:                      /* follow the line-number reference under the cursor */
            do_follow_ref();
            break;

        case K_F6:                      /* find next */
            find_next();
            break;

        case K_F8:                      /* jump back */
            do_jump_back();
            break;

        case K_F9:                      /* PROC/FN outline */
            do_outline();
            break;

        case K_F3:
            do_open();
            break;

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
            static const char *b[] = { "&Save", "&Discard", "&Cancel" };
            int r = dialog("ed", "Save changes before leaving?", b, 3, 0);
            if (r == 0) {                                   /* Save */
                if (!strcmp(filename, "NONAME.TXT")) do_saveas();
                else if (save_file(filename) < 0) { message("Could not save."); quit = 0; }
            } else if (r != 1) quit = 0;                    /* Cancel / Esc: stay in the editor */
        }                                                   /* Discard (r==1): leave, unsaved */

        /* A run of typed characters coalesces into one undo step; any other key
         * (movement, edit, command) ends the run so the next char starts fresh. */
        if (!(c >= 32 && c < 256 && !(mods & (KMOD_CTRL | KMOD_ALT)))) undo_group = 0;

        if (curx < body_off()) curx = body_off();           /* never rest inside the gutter number */
        scroll_into_view();
        redraw();
    }

    free_all();
    S->gfx_backbuffer(0);               /* back to direct drawing */
    S->gfx_clear(0x000000);             /* wipe the WHOLE framebuffer (vdu 12 only clears
                                         * the text cell grid, leaving the partial bottom
                                         * row still painted blue) */
    S->vdu(12);                         /* clear + home the text console for BASIC */
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