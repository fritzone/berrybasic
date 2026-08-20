/* ===========================================================================
 *  debug - a full-screen source-level debugger for BerryBasiC programs.
 *
 *  Shipped to /sys as the `debug` command:
 *
 *      debug PROG.BAS
 *
 *  It loads and runs the program with the interpreter's debugger armed
 *  (dbg_run) and, at every pause - a breakpoint, a step, or an error - paints
 *  the source with the current line highlighted, a variables pane and a
 *  call-stack pane, then a menu bar and status bar exactly like the `ed`
 *  editor. It shares ed's look: a blue framed window, a white menu/status bar,
 *  cyan selection, and the same modal dialogs.
 *
 *  Menus (F10, or Alt+letter):
 *    Debug  - Step Into/Over/Out, Continue, Run to Cursor, Abort
 *    Break  - Toggle, Conditional..., Watch..., List, Clear All
 *    Go     - Go to Line..., Top, Bottom
 *    Help   - Keys, About
 *
 *  Keys:  F7 step  F8 over  F6 out  F5 cont  F9 breakpoint  up/down move
 *         g run-to-cursor   q/Esc abort   F10 menu
 * ======================================================================== */

#include "pod.h"
#include "pod_rt.h"       /* berry_svc: the services table, stashed by crt0 */
#include <string.h>

POD_NAME("debug")
POD_VERSION("1.0")
POD_DESCRIPTION("source-level debugger for BASIC programs")
POD_NEEDS(CAP_DEBUG,    "DEBUG=breakpoints, stepping, variable and stack inspection")
POD_NEEDS(CAP_CONSOLE,  "CONSOLE=reads the keyboard, draws the panes")
POD_NEEDS(CAP_GRAPHICS, "GRAPHICS=double-buffered glyph rendering, so it never flickers")
POD_NEEDS(CAP_VARS,     "VARS=reads variable values to show them")
POD_NEEDS(CAP_SPAWN,    "SPAWN=loads and runs the program under test")

/* --- key codes (as the console delivers them; matches the editor) --------- */
#define K_ENTER  10
#define K_ESC    0x1B
#define K_TAB    9
#define K_BS     8
#define K_LEFT   0x11
#define K_RIGHT  0x12
#define K_UP     0x13
#define K_DOWN   0x14
#define K_HOME   0x15
#define K_END    0x16
#define K_DEL    0x7F
#define K_F4     0x104
#define K_F5     0x105
#define K_F6     0x106
#define K_F7     0x107
#define K_F8     0x108
#define K_F9     0x109
#define K_F10    0x10A
#define K_PGUP   0x10D
#define K_PGDN   0x10E

/* modifier bits from the keymods service */
#define KMOD_SHIFT 0x001
#define KMOD_CTRL  0x002
#define KMOD_ALT   0x004

/* logical colours, as ed uses them */
#define C_BLACK 0
#define C_RED   1
#define C_GREEN 2
#define C_YEL   3
#define C_BLUE  4
#define C_MAG   5
#define C_CYAN  6
#define C_WHITE 7

/* CP437 box-drawing glyphs, as ed */
#define B_TL 201
#define B_TR 187
#define B_BL 200
#define B_BR 188
#define B_H  205
#define B_V  186

static const BerryServices *S;
static int CELLW = 8, CELLH = 16;
static int COLS = 80, ROWS = 25;

/* BBC logical colours 0..7 as 0xRRGGBB, matching the system text palette. */
static const unsigned pal[8] = {
    0x000000, 0xFF0000, 0x00FF00, 0xFFFF00,
    0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF
};
static unsigned cur_fg = 0xFFFFFF, cur_bg = 0x000000;
static int pen_x, pen_y;

static void fg(int c) { cur_fg = pal[c & 7]; }
static void bg(int c) { cur_bg = pal[c & 7]; }
static void fgrgb(unsigned c) { cur_fg = c; }
static void bgrgb(unsigned c) { cur_bg = c; }
static void at(int x, int y) { pen_x = x; pen_y = y; }

static void out(int ch)
{
    if (pen_x >= 0 && pen_x < COLS && pen_y >= 0 && pen_y < ROWS)
        S->con_glyph(pen_x * CELLW, pen_y * CELLH, ch & 0xFF, cur_fg, cur_bg);
    pen_x++;
}
static void cls(void) { S->gfx_clear(cur_bg); }
static int  key(void) { S->gfx_flip(); int c = S->getkey(); return c == 13 ? K_ENTER : c; }

static void caret(int col, int row)
{
    int px = col * CELLW, py = row * CELLH + CELLH - 2;
    S->gfx_fillrect(px, py, px + CELLW - 1, py + 1, cur_fg);
}
static void puts_at(int x, int y, const char *s) { at(x, y); while (*s) out((unsigned char)*s++); }
static void repeat_ch(int x, int y, int ch, int n) { at(x, y); for (int i = 0; i < n; i++) out(ch); }

/* --- tiny string helpers (freestanding) ---------------------------------- */
static void num_str(int v, char *out)
{
    char t[12]; int n = 0, i = 0;
    if (v < 0) { out[i++] = '-'; v = -v; }
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) out[i++] = t[--n]; out[i] = 0;
}
static int str_to_num(const char *s)
{
    int v = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

/* =========================================================================
 * ed-style modal dialogs: a bordered box, buttons, a text prompt, a message.
 * ========================================================================= */

static int dlg_up(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int dlg_disp_len(const char *s) { int n = 0; for (; *s; s++) if (*s != '&') n++; return n; }
static int dlg_btn_w(const char *s) { return dlg_disp_len(s) + 4; }

static void draw_box(int x, int y, int w, int h, const char *title)
{
    bg(C_WHITE); fg(C_BLACK);
    for (int r = 0; r < h; r++) repeat_ch(x, y + r, ' ', w);
    at(x, y); out(B_TL); repeat_ch(x + 1, y, B_H, w - 2); at(x + w - 1, y); out(B_TR);
    for (int r = 1; r < h - 1; r++) { at(x, y + r); out(B_V); at(x + w - 1, y + r); out(B_V); }
    at(x, y + h - 1); out(B_BL); repeat_ch(x + 1, y + h - 1, B_H, w - 2); at(x + w - 1, y + h - 1); out(B_BR);
    if (title && *title) {
        int tl = (int)strlen(title), tx = x + (w - tl - 2) / 2;
        at(tx, y); out(' '); puts_at(tx + 1, y, title); out(' ');
    }
}
static void draw_button(int x, int y, const char *label, int focused)
{
    bg(focused ? C_CYAN : C_WHITE); fg(C_BLACK);
    at(x, y); out('['); out(' ');
    for (const char *p = label; *p; p++) {
        if (*p == '&') { if (p[1]) { fg(C_RED); out((unsigned char)p[1]); fg(C_BLACK); p++; } }
        else out((unsigned char)*p);
    }
    out(' '); out(']');
}

static void repaint(void);        /* forward: the whole debugger screen behind a box */

/* A one-button information/error box. */
static void message(const char *msg)
{
    int msgw = (int)strlen(msg), w = msgw + 6, h = 6, x, y;
    if (w > COLS - 2) w = COLS - 2; if (w < 16) w = 16;
    x = (COLS - w) / 2; y = (ROWS - h) / 2;
    repaint();
    for (;;) {
        draw_box(x, y, w, h, "debug");
        bg(C_WHITE); fg(C_BLACK);
        puts_at(x + (w - msgw) / 2, y + 2, msg);
        draw_button(x + (w - 8) / 2, y + h - 2, "&OK", 1);
        int c = key();
        if (c == K_ESC || c == K_ENTER || c == ' ' || dlg_up(c) == 'O') { repaint(); return; }
    }
}

/* A titled input box with [ OK ] / [ Cancel ]. Returns 1 if OK'd non-empty. */
static int prompt(const char *label, char *buf, int max)
{
    int w = COLS / 2, x, y, n = (int)strlen(buf), focus = 0, h = 7;
    if (w < 34) w = 34; if (w > COLS - 2) w = COLS - 2;
    x = (COLS - w) / 2; y = (ROWS - h) / 2;
    repaint();          /* keep any caller-supplied default already in buf */
    for (;;) {
        int bw0 = dlg_btn_w("&OK"), bw1 = dlg_btn_w("&Cancel"), brow = bw0 + 1 + bw1, bx, by;
        draw_box(x, y, w, h, "debug");
        bg(C_WHITE); fg(C_BLACK);
        puts_at(x + 2, y + 1, label);
        bg(focus == 0 ? C_CYAN : C_WHITE); fg(C_BLACK);
        repeat_ch(x + 2, y + 3, ' ', w - 4);
        puts_at(x + 2, y + 3, buf);
        if (focus == 0) caret(x + 2 + n, y + 3);
        bg(C_WHITE); fg(C_BLACK);
        bx = x + (w - brow) / 2; by = y + h - 2;
        draw_button(bx, by, "&OK", focus == 1);
        draw_button(bx + bw0 + 1, by, "&Cancel", focus == 2);
        int c = key();
        if (c == K_ESC) { repaint(); return 0; }
        if (c == K_ENTER) { repaint(); return focus == 2 ? 0 : (n > 0); }
        if (c == K_TAB) { focus = (focus + 1) % 3; continue; }
        if (dlg_up(c) == 'O' && focus != 0) { repaint(); return n > 0; }
        if (dlg_up(c) == 'C' && focus != 0) { repaint(); return 0; }
        if (focus == 0) {
            if (c == K_BS && n) { buf[--n] = 0; continue; }
            if (c >= 32 && c < 127 && n < max - 1) { buf[n++] = (char)c; buf[n] = 0; }
        } else {
            if (c == ' ') { repaint(); return focus == 2 ? 0 : (n > 0); }
            if (c == K_LEFT)  focus = focus == 2 ? 1 : 0;
            if (c == K_RIGHT) focus = focus == 1 ? 2 : (focus == 0 ? 1 : 2);
        }
    }
}

/* A drop-down list. Returns the chosen index, or NAV_* to move along the bar. */
#define NAV_CANCEL (-1)
#define NAV_LEFT   (-2)
#define NAV_RIGHT  (-3)
static int is_sep(const char *s) { return s[0] == '-' && s[1] == 0; }

static int popup(int x, int y, const char *const *items, int n)
{
    int sel = 0, w = 0;
    for (int i = 0; i < n; i++) { int l = is_sep(items[i]) ? 0 : (int)strlen(items[i]); if (l > w) w = l; }
    w += 2;
    while (sel < n && is_sep(items[sel])) sel++;
    for (;;) {
        for (int i = 0; i < n; i++) {
            if (is_sep(items[i])) { bg(C_WHITE); fg(C_BLACK); at(x, y + i); for (int j = 0; j < w; j++) out(B_H); }
            else { bg(i == sel ? C_CYAN : C_WHITE); fg(C_BLACK); repeat_ch(x, y + i, ' ', w); puts_at(x + 1, y + i, items[i]); }
        }
        int c = key();
        if (c == K_UP)    { do { sel = sel ? sel - 1 : n - 1; } while (is_sep(items[sel])); continue; }
        if (c == K_DOWN)  { do { sel = (sel + 1) % n;         } while (is_sep(items[sel])); continue; }
        if (c == K_ENTER || c == ' ') { if (!is_sep(items[sel])) return sel; continue; }
        if (c == K_ESC)   return NAV_CANCEL;
        if (c == K_LEFT)  return NAV_LEFT;
        if (c == K_RIGHT) return NAV_RIGHT;
    }
}

/* =========================================================================
 * The debugger screen: menu bar, framed source window, side pane, status bar.
 * ========================================================================= */

#define SIDE_W 26                       /* right-hand pane width, in cells */

static const char *g_path;              /* the program being debugged */
static dbg_ctx     g_ctx;               /* the current stop context */
static int         cur_idx;             /* stopped line (program-table index) */
static int         view_cur;            /* the navigation cursor (program-table index) */
static int         view_top;            /* first source line shown */

static int line_count(void) { return S->dbg_line_count(); }
static int cursor_lineno(void);     /* the user line number of the selected line, or -1 */

static int has_break(int ln)
{
    int lines[64]; int n = S->dbg_list_breaks(lines, 64);
    for (int i = 0; i < n; i++) if (lines[i] == ln) return 1;
    return 0;
}

/* --- the menu bar --------------------------------------------------------- */

static const char *m_debug[] = {
    " Step Into      F7 ", " Step Over      F8 ", " Step Out       F6 ",
    " Continue       F5 ", "-", " Run to Line    F4 ", " Abort Program  Esc", 0
};
static const char *m_break[] = {
    " Toggle Break      F9 ", " Conditional...       ", " Watch Variable...    ",
    "-", " List Breakpoints     ", " Clear All            ", 0
};
static const char *m_go[] = {
    " Go to Line...  ", " Top            ", " Bottom         ", 0
};
static const char *m_help[] = { " Keys       ", " About      ", 0 };

static const struct { const char *title; const char *const *items; } menus[] = {
    { " Debug ", m_debug }, { " Break ", m_break }, { " Go ", m_go }, { " Help ", m_help },
};
#define NMENU ((int)(sizeof menus / sizeof menus[0]))

static int menu_col(int i) { int x = 2; for (int j = 0; j < i; j++) x += (int)strlen(menus[j].title); return x; }
static int menu_count(const char *const *it) { int n = 0; while (it[n]) n++; return n; }

static void draw_bar(int active)
{
    bg(C_WHITE); fg(C_BLACK);
    repeat_ch(0, 0, ' ', COLS);
    for (int i = 0; i < NMENU; i++) {
        bg(i == active ? C_CYAN : C_WHITE); fg(C_BLACK);
        puts_at(menu_col(i), 0, menus[i].title);
    }
    bg(C_WHITE); fg(C_BLACK);
    if (COLS >= 28) puts_at(COLS - 28, 0, " debug - BerryBasiC ");
}

/* --- the framed source window + panes ------------------------------------ */

static int win_top, win_bot, src_l, src_r, div_col, side_l;

static void layout(void)
{
    win_top = 1;
    win_bot = ROWS - 2;
    src_l   = 1;
    div_col = COLS - SIDE_W - 1;
    src_r   = div_col - 1;
    side_l  = div_col + 1;
}

static void draw_frame(void)
{
    bg(C_BLUE); fg(C_WHITE);
    at(0, win_top); out(B_TL); repeat_ch(1, win_top, B_H, COLS - 2); at(COLS - 1, win_top); out(B_TR);
    for (int r = win_top + 1; r < win_bot; r++) { at(0, r); out(B_V); at(COLS - 1, r); out(B_V); }
    at(0, win_bot); out(B_BL); repeat_ch(1, win_bot, B_H, COLS - 2); at(COLS - 1, win_bot); out(B_BR);
    /* filename centred in the top border */
    int w = (int)strlen(g_path) + 2, x = (COLS - w) / 2;
    fg(C_YEL); at(x, win_top); out(' '); puts_at(x + 1, win_top, g_path); out(' '); fg(C_WHITE);
    /* vertical divider between source and the side pane */
    for (int r = win_top + 1; r < win_bot; r++) { at(div_col, r); out(B_V); }
    at(div_col, win_top); out(203);     /* T-down */
    at(div_col, win_bot); out(202);     /* T-up   */
}

static void draw_source(void)
{
    int top_row = win_top + 1, nrows = win_bot - win_top - 1;
    int total = line_count();
    if (view_cur < view_top) view_top = view_cur;
    if (view_cur >= view_top + nrows) view_top = view_cur - nrows + 1;
    if (cur_idx >= 0) {
        if (cur_idx < view_top) view_top = cur_idx;
        if (cur_idx >= view_top + nrows) view_top = cur_idx - nrows + 1;
    }
    if (view_top > total - nrows) view_top = total - nrows;
    if (view_top < 0) view_top = 0;

    for (int r = 0; r < nrows; r++) {
        int idx = view_top + r, row = top_row + r, number = 0;
        char text[128], ns[12];
        int is_cur = (idx == cur_idx);      /* the line the program is stopped on */
        int is_sel = (idx == view_cur);     /* the line the user has selected      */
        /* background: stopped line = cyan; selected line = a steel-blue bar;
         * everything else = the window blue. */
        unsigned rowbg = is_cur ? pal[C_CYAN] : (is_sel ? 0x335A8C : pal[C_BLUE]);
        bgrgb(rowbg);
        repeat_ch(src_l, row, ' ', src_r - src_l + 1);
        if (idx < 0 || idx >= total) continue;
        if (S->dbg_line_at(idx, &number, text, sizeof text) < 0) continue;
        /* gutter: breakpoint dot, then a marker - the stopped line gets a filled
         * arrow, the selected line a hollow one, so both are visible at once. */
        if (has_break(number)) { fgrgb(pal[C_RED]); at(src_l, row); out('*'); }
        if (is_cur)      { fg(C_BLACK); at(src_l + 1, row); out(16); }   /* filled arrow */
        else if (is_sel) { fgrgb(pal[C_YEL]); at(src_l + 1, row); out(175); } /* hollow arrow */
        num_str(number, ns);
        if (is_cur) fg(C_BLACK); else fg(C_CYAN);
        { int k = 0; at(src_l + 2, row); while (ns[k]) out((unsigned char)ns[k++]); out(' '); }
        if (is_cur) fg(C_BLACK); else fg(C_WHITE);
        { int c0 = src_l + 2 + (int)strlen(ns) + 1, k = 0;
          at(c0, row); while (text[k] && c0 + k <= src_r) { out((unsigned char)text[k]); k++; } }
    }
}

static void fmt_val(const char *name, int isstr, char *out, int outsz)
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

static void draw_side(void)
{
    int row = win_top + 1, maxrow = win_bot - 1;
    bg(C_BLUE);
    for (int r = row; r <= maxrow; r++) repeat_ch(side_l, r, ' ', COLS - 1 - side_l);
    fg(C_YEL); puts_at(side_l, row++, "VARIABLES");
    int nv = S->dbg_var_count(), half = (maxrow - win_top) / 2;
    for (int i = 0; i < nv && row < win_top + 1 + half; i++) {
        char name[16]; int isstr = 0, isarr = 0;
        if (S->dbg_var_at(i, name, sizeof name, &isstr, &isarr) < 0) continue;
        char val[40], line[52];
        if (isarr) strcpy(val, "(array)"); else fmt_val(name, isstr, val, sizeof val);
        strcpy(line, name); strcat(line, "="); strncat(line, val, 36);
        line[COLS - 1 - side_l] = 0;
        fg(C_WHITE); puts_at(side_l, row++, line);
    }
    row++;
    fg(C_YEL); puts_at(side_l, row++, "CALL STACK");
    int depth = S->dbg_stack_depth();
    if (depth == 0) { fg(C_CYAN); puts_at(side_l, row++, " (top level)"); }
    for (int i = 0; i < depth && row <= maxrow; i++) {
        char name[16], ns[12], line[40]; int cl = 0;
        if (S->dbg_stack_frame(i, name, sizeof name, &cl) < 0) break;
        strcpy(line, " "); strcat(line, name); strcat(line, " @"); num_str(cl, ns); strcat(line, ns);
        fg(C_CYAN); puts_at(side_l, row++, line);
    }
}

static const char *event_name(int ev)
{
    switch (ev) {
        case BERRY_DBG_ERROR:   return "error";
        case BERRY_DBG_CALL:    return "call";
        case BERRY_DBG_WRITE:   return "write";
        case BERRY_DBG_KEYWORD: return "keyword";
        case BERRY_DBG_STMT:    return "statement";
        default:                return "stopped";
    }
}

static void draw_status(void)
{
    char b[128], ns[12]; int n;
    bg(C_WHITE); fg(C_BLACK);
    repeat_ch(0, ROWS - 1, ' ', COLS);
    /* left: stop reason + line */
    strcpy(b, "  ");
    strcat(b, event_name(g_ctx.event));
    if (g_ctx.line >= 0) { strcat(b, " at line "); num_str(g_ctx.line, ns); strcat(b, ns); }
    if (g_ctx.event == BERRY_DBG_ERROR && g_ctx.errmsg) { strcat(b, ": "); strncat(b, g_ctx.errmsg, 40); }
    else if ((g_ctx.event == BERRY_DBG_CALL || g_ctx.event == BERRY_DBG_WRITE) && g_ctx.name) {
        strcat(b, " "); strncat(b, g_ctx.name, 16);
    }
    /* selected line, so it is obvious which line Run-to-Line will target */
    { int sl = cursor_lineno(); if (sl >= 0) { strcat(b, "  sel "); num_str(sl, ns); strcat(b, ns); } }
    puts_at(0, ROWS - 1, b);
    /* right: key hints */
    n = 52;
    if (COLS >= n) puts_at(COLS - n, ROWS - 1, " up/dn select  F4 run-to-line  F9 bp  F10 menu ");
}

static void repaint(void)
{
    bg(C_BLUE); cls();
    draw_bar(-1);
    draw_frame();
    draw_source();
    draw_side();
    draw_status();
}

/* =========================================================================
 * Breakpoint operations reached from the menu.
 * ========================================================================= */

/* the user line number of the navigation-cursor line, or -1 */
static int cursor_lineno(void)
{
    int number = 0; char t[8];
    if (view_cur >= 0 && S->dbg_line_at(view_cur, &number, t, sizeof t) == 0) return number;
    return -1;
}

static void bp_toggle(void)
{
    int ln = cursor_lineno();
    if (ln < 0) { message("This line has no line number."); return; }
    if (has_break(ln)) S->dbg_clear(ln); else S->dbg_break_line(ln);
}

static void bp_conditional(void)
{
    char lnbuf[16], cond[80];
    int ln = cursor_lineno();
    num_str(ln < 0 ? 0 : ln, lnbuf);
    if (!prompt(" Break at line number:", lnbuf, sizeof lnbuf)) return;
    ln = str_to_num(lnbuf);
    if (ln <= 0) { message("Not a valid line number."); return; }
    cond[0] = 0;
    if (!prompt(" Condition (e.g. I=5), blank = always:", cond, sizeof cond)) {
        /* blank condition = an ordinary breakpoint */
        S->dbg_break_line(ln); return;
    }
    if (cond[0]) S->dbg_break_line_if(ln, cond, 0);
    else         S->dbg_break_line(ln);
}

static void bp_watch(void)
{
    char name[16], cond[80];
    name[0] = 0;
    if (!prompt(" Watch which variable? (e.g. TOTAL)", name, sizeof name)) return;
    cond[0] = 0;
    prompt(" Only when (blank = any write):", cond, sizeof cond);
    S->dbg_watch(name, cond);
    message("Watch set.");
}

static void bp_list(void)
{
    int lines[64], n = S->dbg_list_breaks(lines, 64);
    char msg[128], ns[12];
    if (n == 0) { message("No line breakpoints set."); return; }
    strcpy(msg, "Breakpoints:");
    for (int i = 0; i < n && (int)strlen(msg) < 100; i++) { strcat(msg, " "); num_str(lines[i], ns); strcat(msg, ns); }
    message(msg);
}

/* =========================================================================
 * The menu bar interaction. Returns 1 if a resume was chosen (leave on_stop).
 * ========================================================================= */

static int do_menu_item(int menu, int item)
{
    if (menu == 0) {                    /* Debug */
        switch (item) {
            case 0: S->dbg_step();      return 1;
            case 1: S->dbg_step_over(); return 1;
            case 2: S->dbg_step_out();  return 1;
            case 3: S->dbg_cont();      return 1;
            case 5: { int ln = cursor_lineno(); if (ln >= 0) { S->dbg_break_line(ln); S->dbg_cont(); return 1; } break; }
            case 6: S->dbg_abort();     return 1;
        }
    } else if (menu == 1) {             /* Break */
        switch (item) {
            case 0: bp_toggle();      break;
            case 1: bp_conditional(); break;
            case 2: bp_watch();       break;
            case 4: bp_list();        break;
            case 5: S->dbg_clear(0);  break;
        }
    } else if (menu == 2) {             /* Go */
        switch (item) {
            case 0: { char b[16]; b[0] = 0; if (prompt(" Go to line number:", b, sizeof b)) {
                        int want = str_to_num(b), total = line_count();
                        for (int i = 0; i < total; i++) { int num = 0; char t[8];
                            if (S->dbg_line_at(i, &num, t, sizeof t) == 0 && num >= want) { view_cur = i; break; } }
                    } break; }
            case 1: view_cur = 0; break;
            case 2: view_cur = line_count() - 1; break;
        }
    } else {                            /* Help */
        if (item == 0) message("up/down select a line - F4 run to it - F7 step F8 over F6 out F5 cont F9 breakpoint q abort");
        else           message("debug - BerryBasiC source-level debugger");
    }
    return 0;
}

/* Open the menu bar starting on `active`. Returns 1 if a resume was chosen. */
static int run_menu(int active)
{
    for (;;) {
        repaint();
        draw_bar(active);
        int n = menu_count(menus[active].items);
        int col = menu_col(active);
        if (col + 24 > COLS) col = COLS - 24;
        int sel = popup(col, 1, menus[active].items, n);
        if (sel == NAV_CANCEL) { repaint(); return 0; }
        if (sel == NAV_LEFT)   { active = (active + NMENU - 1) % NMENU; continue; }
        if (sel == NAV_RIGHT)  { active = (active + 1) % NMENU; continue; }
        int resume = do_menu_item(active, sel);
        return resume;
    }
}

/* Which menu an Alt+letter opens (Alt+D/B/G/H). -1 if none. */
static int menu_accel(int c)
{
    int up = dlg_up(c);
    for (int i = 0; i < NMENU; i++) {
        const char *t = menus[i].title; while (*t == ' ') t++;
        if (dlg_up(*t) == up) return i;
    }
    return -1;
}

/* =========================================================================
 * The stop handler: the interactive UI at each pause.
 * ========================================================================= */

static void on_stop(const dbg_ctx *c)
{
    g_ctx = *c;
    cur_idx = c->line_idx;
    view_cur = c->line_idx;
    for (;;) {
        repaint();
        int k = key();
        int mods = S->keymods();
        int nrows = win_bot - win_top - 1;
        int am;
        if ((mods & KMOD_ALT) && (am = menu_accel(k)) >= 0) { if (run_menu(am)) return; continue; }
        switch (k) {
            case K_F7: case 's':               S->dbg_step();      return;
            case K_F8: case 'o':               S->dbg_step_over(); return;
            case K_F6: case 'u':               S->dbg_step_out();  return;
            case K_F5: case 'c': case K_ENTER: S->dbg_cont();      return;
            case 'q': case K_ESC:              S->dbg_abort();     return;
            case K_F10:                        if (run_menu(0)) return; break;
            case K_UP:   if (view_cur > 0) view_cur--; break;
            case K_DOWN: if (view_cur < line_count() - 1) view_cur++; break;
            case K_PGUP: view_cur -= nrows; if (view_cur < 0) view_cur = 0; break;
            case K_PGDN: view_cur += nrows; if (view_cur > line_count() - 1) view_cur = line_count() - 1; break;
            case K_HOME: view_cur = 0; break;
            case K_END:  view_cur = line_count() - 1; break;
            case K_F9:   bp_toggle(); break;
            case K_F4: case 'g':               /* run to the selected line */
                { int ln = cursor_lineno(); if (ln >= 0) { S->dbg_break_line(ln); S->dbg_cont(); return; } break; }
            default: break;
        }
    }
}

int main(int argc, char **argv)
{
    S = berry_svc;
    if (argc < 2) { S->puts("usage: debug PROG.BAS\n", 22); return 1; }
    g_path = argv[1];

    int cw = 0, ch = 0;
    S->con_font(&cw, &ch);
    if (cw > 0) CELLW = cw;
    if (ch > 0) CELLH = ch;
    int pcols = 0, prows = 0;
    S->screen_size(&pcols, &prows);
    if (pcols > 0) COLS = pcols;
    if (prows > 0) ROWS = prows;
    layout();

    /* Arm: attach the UI and stop before the first line, so the user can set
     * breakpoints; break on an uncaught error too; then run under the debugger. */
    S->dbg_attach(on_stop);
    S->dbg_break_error(1);
    S->dbg_break_every(1);

    S->gfx_backbuffer(1);
    int r = S->dbg_run(g_path);
    S->dbg_detach();

    g_ctx.event = -1; g_ctx.line = -1; g_ctx.errmsg = 0;
    cur_idx = -1;
    repaint();
    bg(C_WHITE); fg(C_BLACK);
    repeat_ch(0, ROWS - 1, ' ', COLS);
    puts_at(0, ROWS - 1, "  program finished - press a key to leave the debugger");
    S->gfx_flip();
    S->getkey();
    return r < 0 ? 1 : 0;
}
