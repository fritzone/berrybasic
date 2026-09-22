/* ===========================================================================
 *  helpview (hview) - a full-screen browser for the BerryBasiC help pages.
 *
 *      hview [TOPIC]      open TOPIC (default: the Index) and browse
 *
 *  Phase 2 of the help system: it lays out the HTML-ish help files (see
 *  helpfmt.h) with headings, word-wrapped prose, syntax-coloured examples and
 *  links between pages, in the editor's own colour scheme. Tab/Shift-Tab move
 *  between links and runnable examples, Enter follows a link or runs the
 *  selected example, F5 runs it too, Backspace goes back, Esc quits.
 * ======================================================================== */
#include "pod.h"
#include "pod_rt.h"       /* berry_svc */
#include <string.h>
#include "../help/helpfmt.h"

POD_NAME("helpview")
POD_VERSION("1.1")
POD_DESCRIPTION("browse the BASIC help pages with runnable examples")
POD_NEEDS(CAP_GRAPHICS, "GRAPHICS=draws the pages, double-buffered")
POD_NEEDS(CAP_CONSOLE,  "CONSOLE=reads the keyboard")
POD_NEEDS(CAP_FILES,    "FILES=reads help pages and writes the example to run")
POD_NEEDS(CAP_HEAP,     "HEAP=the C runtime that reads the files")
POD_NEEDS(CAP_SPAWN,    "SPAWN=runs an example with run_basic")

/* --- keys --- */
#define K_ENTER 10
#define K_ESC   0x1B
#define K_BS    8
#define K_TAB   9
#define K_UP    0x13
#define K_DOWN  0x14
#define K_HOME  0x15
#define K_END   0x16
#define K_F5    0x105
#define K_PGUP  0x10D
#define K_PGDN  0x10E
#define KMOD_SHIFT 0x001

static const BerryServices *S;
static int CELLW = 8, CELLH = 16, COLS = 80, ROWS = 25;

/* colours to match the ed editor: blue window, white bars, ed's syntax palette */
#define BG      0x0000FF   /* the editor window blue (C_BLUE)              */
#define FG      0xFFFFFF   /* body text                                   */
#define H1COL   0xFFE060   /* ed keyword yellow - headings                */
#define H2COL   0x50E0FF   /* ed line-number cyan - subheadings           */
#define LINKCOL 0x80FF80   /* ed string green - links                     */
#define BARBG   0xFFFFFF   /* ed bars: white bg, black text               */
#define BARFG   0x000000
#define SELBG   0x00FFFF   /* ed selection: cyan bg + black text (a link) */
#define EXBG    0x000098   /* a selected, runnable example block          */

/* CP437 double-line box, as the editor's frame */
#define B_TL 201
#define B_TR 187
#define B_BL 200
#define B_BR 188
#define B_H  205
#define B_V  186

/* code-line syntax colours (per char class), ed's exact values */
enum { CC_TEXT=0, CC_KW, CC_STR, CC_NUM, CC_COM, CC_LNO };
static unsigned code_col(int cc) {
    switch (cc) { case CC_KW: return 0xFFE060; case CC_STR: return 0x80FF80;
        case CC_NUM: return 0xFF9C60; case CC_COM: return 0x9FB0D0;
        case CC_LNO: return 0x50E0FF; default: return 0xFFFFFF; }
}

/* --- glyph output --- */
static void glyph(int col, int row, int ch, unsigned fg, unsigned bg) {
    if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return;
    S->con_glyph(col * CELLW, row * CELLH, ch & 0xFF, fg, bg);
}
static void puts_at(int col, int row, const char *s, unsigned fg, unsigned bg) {
    for (; *s && col < COLS; s++, col++) glyph(col, row, (unsigned char)*s, fg, bg);
}
static void fill(int row, int c0, int c1, unsigned bg) { for (int c = c0; c < c1; c++) glyph(c, row, ' ', FG, bg); }
static int key(void) { S->gfx_flip(); int c = S->getkey(); return c == 13 ? K_ENTER : c; }

/* --- the editor-style framed window ---------------------------------------
 * Row 0 is a bar; rows win_y1-1 .. win_y2+1 are the bordered window (content in
 * win_y1..win_y2, columns win_x1..win_x2); the last row is the status bar. */
static int win_x1, win_y1, win_x2, win_y2;
static char g_title[40];

static void set_layout(void) {
    win_x1 = 1; win_y1 = 2; win_x2 = COLS - 2; win_y2 = ROWS - 3;
    if (win_y2 < win_y1) win_y2 = win_y1;
}
static int content_rows(void) { return win_y2 - win_y1 + 1; }

static void draw_frame(void) {
    /* top border */
    glyph(win_x1 - 1, win_y1 - 1, B_TL, FG, BG);
    for (int c = win_x1; c <= win_x2; c++) glyph(c, win_y1 - 1, B_H, FG, BG);
    glyph(win_x2 + 1, win_y1 - 1, B_TR, FG, BG);
    /* sides */
    for (int r = win_y1; r <= win_y2; r++) { glyph(win_x1 - 1, r, B_V, FG, BG); glyph(win_x2 + 1, r, B_V, FG, BG); }
    /* bottom border */
    glyph(win_x1 - 1, win_y2 + 1, B_BL, FG, BG);
    for (int c = win_x1; c <= win_x2; c++) glyph(c, win_y2 + 1, B_H, FG, BG);
    glyph(win_x2 + 1, win_y2 + 1, B_BR, FG, BG);
    /* topic title centred in the top border, like the editor's filename */
    int tl = (int)strlen(g_title), w = tl + 2, x = win_x1 + (win_x2 - win_x1 + 1 - w) / 2;
    if (x < win_x1) x = win_x1;
    glyph(x, win_y1 - 1, ' ', H1COL, BG);
    puts_at(x + 1, win_y1 - 1, g_title, H1COL, BG);
    glyph(x + 1 + tl, win_y1 - 1, ' ', H1COL, BG);
}

/* =========================================================================
 * Laid-out document: display lines (char + colour class), a link table, and an
 * example table (line ranges of runnable <code> blocks). A unified item list
 * (links + examples in document order) drives Tab navigation.
 * ========================================================================= */
#define MAXLINES 600
#define MAXW     200
#define MAXLINKS 400
#define MAXEX    48
static char          Ltext[MAXLINES][MAXW];
static unsigned char Lcol[MAXLINES][MAXW];   /* prose: style byte; code: unused */
static short         Llen[MAXLINES];
static unsigned char Lcode[MAXLINES];        /* 1 = a code line */
static short         Lex[MAXLINES];          /* example index for a runnable code line, else -1 */
static int           nlines;
static struct { short line, col, len; char href[24]; } links[MAXLINKS];
static int           nlinks;
static short         ex_l0[MAXEX], ex_l1[MAXEX];
static int           n_ex;
static struct { unsigned char kind; short ref; short line; } items[MAXLINKS + MAXEX];  /* kind 0=link 1=example */
static int           n_items;

static unsigned prose_col(int st) {
    switch (st) { case 1: return H1COL; case 2: return H2COL; case 3: return LINKCOL; default: return FG; }
}

static int layw, cur_col, last_blank;
/* code-block layout state */
static int in_ex, cur_ex, ex_started;

static void L_newline(void) {
    if (nlines >= MAXLINES - 1) return;
    Llen[nlines] = cur_col; nlines++;
    cur_col = 0; Lcode[nlines] = 0; Lex[nlines] = -1;
}
static void L_flush(void) { if (cur_col > 0) L_newline(); }
static void L_blank(void) { L_flush(); if (!last_blank && nlines < MAXLINES - 1) { Llen[nlines] = 0; Lcode[nlines] = 0; Lex[nlines] = -1; nlines++; last_blank = 1; } }
static void L_putc(int ch, int style) {
    if (cur_col >= MAXW - 1) return;
    Ltext[nlines][cur_col] = (char)ch; Lcol[nlines][cur_col] = (unsigned char)style; cur_col++; last_blank = 0;
}
static void L_word(const char *w, int n, int style, const char *href) {
    if (n <= 0) return;
    if (cur_col > 0 && cur_col + 1 + n > layw) L_newline();
    if (cur_col > 0) L_putc(' ', 0);
    int start = cur_col;
    for (int i = 0; i < n; i++) L_putc((unsigned char)w[i], style);
    if (href && nlinks < MAXLINKS) {
        links[nlinks].line = (short)nlines; links[nlinks].col = (short)start; links[nlinks].len = (short)n;
        int j = 0; for (; href[j] && j < 23; j++) links[nlinks].href[j] = href[j]; links[nlinks].href[j] = 0;
        nlinks++;
    }
}
static void L_words(const char *s, int n, int style, const char *href) {
    int i = 0;
    while (i < n) {
        while (i < n && (s[i]==' '||s[i]=='\n'||s[i]=='\t'||s[i]=='\r')) i++;
        int j = i; while (j < n && s[j]!=' '&&s[j]!='\n'&&s[j]!='\t'&&s[j]!='\r') j++;
        if (j > i) L_word(s + i, j - i, style, href);
        i = j;
    }
}

/* --- the layout sink --- */
static void s_title(void *cx, const char *s, int n) { (void)cx; int i=0; for(;i<n&&i<39;i++) g_title[i]=s[i]; g_title[i]=0; }
static void s_heading(void *cx, int lvl, const char *s, int n) {
    (void)cx; L_flush(); L_blank();
    L_words(s, n, lvl == 1 ? 1 : 2, 0); L_flush();
    if (lvl == 1) { for (int i = 0; i < n && i < layw; i++) L_putc('=', 1); L_flush(); }
    last_blank = 0; L_blank();
}
static void s_para_begin(void *cx) { (void)cx; L_flush(); }
static void s_run(void *cx, const char *s, int n, int style) { (void)cx; L_words(s, n, (style & HS_BOLD) ? 1 : 0, 0); }
static void s_link(void *cx, const char *href, int hn, const char *label, int ln) {
    (void)cx; char h[24]; int i=0; for(;i<hn&&i<23;i++) h[i]=href[i]; h[i]=0; L_words(label, ln, 3, h);
}
static void s_para_end(void *cx) { (void)cx; L_flush(); L_blank(); }
static void s_code_begin(void *cx, int ex) {
    (void)cx; L_flush(); L_blank();
    in_ex = (ex && n_ex < MAXEX); cur_ex = in_ex ? n_ex : -1; ex_started = 0;
}
static void s_code_line(void *cx, const char *s, int n) {
    (void)cx; L_flush();
    Lcode[nlines] = 1; Lex[nlines] = (short)(in_ex ? cur_ex : -1);
    if (in_ex) { if (!ex_started) { ex_l0[cur_ex] = (short)nlines; ex_started = 1; } ex_l1[cur_ex] = (short)nlines; }
    L_putc(' ', 0); L_putc(' ', 0);
    for (int i = 0; i < n && cur_col < MAXW - 1; i++) L_putc((unsigned char)s[i], 0);
    L_newline();
}
static void s_code_end(void *cx) { (void)cx; if (in_ex) n_ex++; in_ex = 0; L_blank(); }
static void s_rule(void *cx) { (void)cx; L_flush(); L_blank(); for (int i=0;i<layw && i<60;i++) L_putc('-',0); L_flush(); L_blank(); }

/* --- BASIC syntax classing for a code line --- */
static int hv_up(int c){ return (c>='a'&&c<='z')?c-32:c; }
static int hv_alpha(int c){ return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_'; }
static int hv_digit(int c){ return c>='0'&&c<='9'; }
static const char *const HVKW[] = {
    "PRINT","INPUT","IF","THEN","ELSE","ENDIF","FOR","TO","STEP","NEXT","WHILE",
    "ENDWHILE","REPEAT","UNTIL","GOTO","GOSUB","RETURN","END","STOP","LET","DIM",
    "DATA","READ","RESTORE","DEF","PROC","FN","ENDPROC","LOCAL","CLS","MODE","GCOL",
    "COLOUR","COLOR","PLOT","DRAW","MOVE","LINE","RECTANGLE","CIRCLE","FILL","VDU",
    "CASE","OF","WHEN","OTHERWISE","ENDCASE","AND","OR","NOT","MOD","DIV","STRING$",
    "LEFT$","RIGHT$","MID$","LEN","CHR$","ASC","ABS","INT","RND","SQR","SIN","PI", 0
};
static int hv_iskw(const char *w, int n) {
    for (int k=0; HVKW[k]; k++){ int j=0; for(;j<n && HVKW[k][j] && hv_up(w[j])==HVKW[k][j]; j++); if (j==n && HVKW[k][j]==0) return 1; }
    return 0;
}
static void hv_class(const char *s, int n, unsigned char *cls) {
    int i = 0;
    while (i < n && s[i] == ' ') cls[i++] = CC_TEXT;
    while (i < n && hv_digit(s[i])) cls[i++] = CC_LNO;
    while (i < n) {
        int c = (unsigned char)s[i];
        if (c == '"') { cls[i++] = CC_STR; while (i<n && s[i] != '"') cls[i++]=CC_STR; if (i<n) cls[i++]=CC_STR; }
        else if (hv_alpha(c)) {
            int j=i; while (j<n && (hv_alpha(s[j])||hv_digit(s[j])||s[j]=='$')) j++;
            if (j-i==3 && hv_up(s[i])=='R'&&hv_up(s[i+1])=='E'&&hv_up(s[i+2])=='M') { while(i<n) cls[i++]=CC_COM; }
            else { int kw = hv_iskw(s+i, j-i); while (i<j) cls[i++] = kw?CC_KW:CC_TEXT; }
        }
        else if (hv_digit(c)) { while (i<n && (hv_digit(s[i])||s[i]=='.')) cls[i++]=CC_NUM; }
        else cls[i++] = CC_TEXT;
    }
}

/* --- load + lay out a topic, then build the item list --- */
static char g_doc[32768];
static int sel;        /* index into items[], or -1 */
static int top;

static void build_items(void) {
    n_items = 0;
    for (int li = 0; li < nlines; li++) {
        for (int e = 0; e < n_ex; e++)
            if (ex_l0[e] == li && n_items < (int)(sizeof items/sizeof items[0]))
                { items[n_items].kind=1; items[n_items].ref=(short)e; items[n_items].line=(short)li; n_items++; }
        for (int L = 0; L < nlinks; L++)
            if (links[L].line == li && n_items < (int)(sizeof items/sizeof items[0]))
                { items[n_items].kind=0; items[n_items].ref=(short)L; items[n_items].line=(short)li; n_items++; }
    }
}
static int layout_topic(const char *topic) {
    int n = help_load(topic, g_doc, sizeof g_doc);
    nlines = 0; nlinks = 0; n_ex = 0; cur_col = 0; last_blank = 1; in_ex = 0;
    Lcode[0] = 0; Lex[0] = -1; g_title[0] = 0;
    layw = win_x2 - win_x1 + 1; if (layw > MAXW - 1) layw = MAXW - 1; if (layw < 10) layw = 10;
    if (n < 0) { strcpy(g_title, topic); L_words("No help page for this topic.", 28, 0, 0); L_flush(); build_items(); return -1; }
    help_sink sk = { 0, s_title, s_heading, s_para_begin, s_run, s_link, s_para_end,
                     s_code_begin, s_code_line, s_code_end, s_rule };
    help_parse(g_doc, &sk);
    L_flush();
    if (!g_title[0]) strcpy(g_title, topic);
    build_items();
    return 0;
}

/* =========================================================================
 * Run the selected example: write its BASIC lines to a temp file and run it.
 * ========================================================================= */
#define TMPBAS "/SYS/HELP/RUN.BAS"
static void run_example(int e) {
    if (e < 0 || e >= n_ex) return;
    int fh = S->file_open(TMPBAS, BERRY_FOPEN_WRITE);
    if (fh <= 0) return;
    for (int li = ex_l0[e]; li <= ex_l1[e] && li < nlines; li++) {
        int start = (Llen[li] >= 2 && Ltext[li][0]==' ' && Ltext[li][1]==' ') ? 2 : 0;  /* drop the 2-space indent */
        if (Llen[li] - start > 0) S->file_write(fh, Ltext[li] + start, Llen[li] - start);
        S->file_write(fh, "\n", 1);
    }
    S->file_close(fh);

    S->gfx_backbuffer(0);              /* run on the visible screen */
    S->run_basic(TMPBAS);
    S->puts("\n[ press a key to return to help ]\n", 34);
    S->getkey();
    S->file_remove(TMPBAS);
    S->gfx_backbuffer(1);
}

/* =========================================================================
 * The viewer
 * ========================================================================= */
static void draw(void)
{
    int crows = content_rows();
    int sel_ex   = (sel >= 0 && sel < n_items && items[sel].kind == 1) ? items[sel].ref : -1;
    int sel_link = (sel >= 0 && sel < n_items && items[sel].kind == 0) ? items[sel].ref : -1;

    S->gfx_clear(BG);
    /* top bar (like the editor's menu bar) */
    fill(0, 0, COLS, BARBG);
    puts_at(0, 0, " Help ", BARFG, BARBG);
    puts_at(COLS > 26 ? COLS - 26 : 6, 0, " BerryBasiC Help Browser ", BARFG, BARBG);
    /* the framed window with the topic title in its top border */
    draw_frame();

    /* content */
    for (int r = 0; r < crows; r++) {
        int li = top + r, row = win_y1 + r;
        if (li < 0 || li >= nlines) continue;
        int len = Llen[li];
        int isexrow = (sel_ex >= 0 && Lex[li] == sel_ex);       /* part of the selected example */
        unsigned rowbg = isexrow ? EXBG : BG;
        if (isexrow) fill(row, win_x1, win_x2 + 1, EXBG);
        if (Lcode[li]) {
            unsigned char cls[MAXW];
            hv_class(Ltext[li], len, cls);
            if (isexrow) glyph(win_x1, row, 16, H1COL, EXBG);   /* a run marker */
            for (int c = 0; c < len && win_x1 + c <= win_x2; c++)
                glyph(win_x1 + c, row, (unsigned char)Ltext[li][c], code_col(cls[c]), rowbg);
        } else {
            for (int c = 0; c < len && win_x1 + c <= win_x2; c++)
                glyph(win_x1 + c, row, (unsigned char)Ltext[li][c], prose_col(Lcol[li][c]), rowbg);
        }
    }
    /* highlight the selected link */
    if (sel_link >= 0) {
        int li = links[sel_link].line;
        if (li >= top && li < top + crows) {
            int row = win_y1 + (li - top);
            for (int c = 0; c < links[sel_link].len; c++) {
                int cc = links[sel_link].col + c;
                if (win_x1 + cc <= win_x2)
                    glyph(win_x1 + cc, row, (unsigned char)Ltext[li][cc], BARFG, SELBG);
            }
        }
    }
    /* status bar */
    fill(ROWS - 1, 0, COLS, BARBG);
    if (sel_ex >= 0)
        puts_at(0, ROWS - 1, " Enter/F5 run this example   Tab move   Bksp back   Esc quit", BARFG, BARBG);
    else
        puts_at(0, ROWS - 1, " Tab move   Enter follow link   F5 run example   Bksp back   Esc quit", BARFG, BARBG);
}

static void ensure_sel_visible(void) {
    int crows = content_rows();
    if (sel < 0 || sel >= n_items) return;
    int li = items[sel].line;
    if (li < top) top = li;
    if (li >= top + crows) top = li - crows + 1;
    if (top < 0) top = 0;
}

/* history of visited topics for Backspace */
static char hist[32][24];
static int  hist_n;
static void go(const char *topic, int push) {
    if (push && hist_n < 32 && g_title[0]) { int i=0; for(;g_title[i]&&i<23;i++) hist[hist_n][i]=g_title[i]; hist[hist_n][i]=0; hist_n++; }
    layout_topic(topic);
    top = 0; sel = (n_items > 0) ? 0 : -1;
}

int main(int argc, char **argv)
{
    S = berry_svc;
    int cw = 0, ch = 0; S->con_font(&cw, &ch); if (cw>0) CELLW=cw; if (ch>0) CELLH=ch;
    int pcols=0, prows=0; S->screen_size(&pcols, &prows); if (pcols>0) COLS=pcols; if (prows>0) ROWS=prows;
    set_layout();

    S->gfx_backbuffer(1);
    const char *start = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : "INDEX";
    layout_topic(start);
    top = 0; sel = (n_items > 0) ? 0 : -1;

    for (;;) {
        int crows = content_rows();
        draw();
        int k = key();
        int mods = S->keymods();
        if (k == K_ESC) break;
        else if (k == K_TAB) {
            if (n_items > 0) {
                if (mods & KMOD_SHIFT) sel = (sel <= 0) ? n_items - 1 : sel - 1;
                else                   sel = (sel + 1) % n_items;
                ensure_sel_visible();
            }
        }
        else if (k == K_ENTER) {
            if (sel >= 0 && sel < n_items) {
                if (items[sel].kind == 0) go(links[items[sel].ref].href, 1);
                else                      run_example(items[sel].ref);
            }
        }
        else if (k == K_F5) {
            int e = (sel >= 0 && items[sel].kind == 1) ? items[sel].ref : (n_ex > 0 ? 0 : -1);
            if (e >= 0) run_example(e);
        }
        else if (k == K_BS) { if (hist_n > 0) { hist_n--; go(hist[hist_n], 0); } }
        else if (k == K_UP)   { if (top > 0) top--; }
        else if (k == K_DOWN) { if (top < nlines - 1) top++; }
        else if (k == K_PGUP) { top -= crows; if (top < 0) top = 0; }
        else if (k == K_PGDN) { top += crows; if (top > nlines - 1) top = nlines - 1; if (top<0) top=0; }
        else if (k == K_HOME) { top = 0; }
        else if (k == K_END)  { top = nlines - crows; if (top < 0) top = 0; }
    }

    S->gfx_backbuffer(0);
    S->gfx_clear(0x000000);
    S->vdu(12);
    return 0;
}
