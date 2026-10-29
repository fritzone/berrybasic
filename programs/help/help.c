/* ===========================================================================
 *  help - print the reference help for a BASIC command to the console.
 *
 *      help PRINT          show the help for PRINT and exit
 *      help                show the index of topics
 *
 *  Help pages are small HTML-ish files in /SYS/HELP/BASIC (see helpfmt.h). This
 *  is phase 1 of the help system: a plain console printer. The graphical browser
 *  (links, syntax-coloured examples) is `hview`.
 * ======================================================================== */
#include "pod.h"
#include "pod_rt.h"        /* berry_svc */
#include <string.h>
#include "helpfmt.h"

POD_NAME("help")
POD_VERSION("1.0")
POD_DESCRIPTION("show reference help for a BASIC command")
POD_NEEDS(CAP_CONSOLE, "CONSOLE=prints the help, sized to the screen")
POD_NEEDS(CAP_FILES,   "FILES=reads the help pages from /SYS/HELP/BASIC")
POD_NEEDS(CAP_HEAP,    "HEAP=the C runtime that reads the files")

static const BerryServices *S;

/* logical colours */
#define C_H1   3   /* yellow */
#define C_H2   6   /* cyan   */
#define C_CODE 2   /* green  */
#define C_LINK 5   /* magenta */
#define C_TEXT 7   /* white  */

static void puts0(const char *s)   { S->puts(s, (int)strlen(s)); }
static void setcol(int c)          { S->vdu(17); S->vdu(c); }
static void nl(void)               { S->putc('\n'); }

/* ---- the console sink: word-wraps prose, indents code, colours headings --- */
typedef struct { int width; int col; } ccon;

static void hp_word(ccon *c, const char *w, int n, int color)
{
    if (n == 0) return;
    if (c->col > 0 && c->col + 1 + n > c->width) { nl(); c->col = 0; }
    if (c->col > 0) { S->putc(' '); c->col++; }
    setcol(color); S->puts(w, n); setcol(C_TEXT);
    c->col += n;
}
static void hp_words(ccon *c, const char *s, int n, int color)
{
    int i = 0;
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r')) i++;
        int j = i;
        while (j < n && s[j] != ' ' && s[j] != '\n' && s[j] != '\t' && s[j] != '\r') j++;
        if (j > i) hp_word(c, s + i, j - i, color);
        i = j;
    }
}

static void c_title(void *cx, const char *s, int n) { (void)cx; (void)s; (void)n; }  /* metadata */
static void c_heading(void *cx, int lvl, const char *s, int n)
{
    ccon *c = cx;
    if (c->col) { nl(); c->col = 0; }
    nl();
    if (lvl == 1) {
        setcol(C_H1); S->puts(s, n); nl();
        for (int i = 0; i < n; i++) S->putc('=');
        setcol(C_TEXT); nl();
    } else {
        setcol(C_H2); S->putc(' '); S->puts(s, n); setcol(C_TEXT); nl();
    }
}
static void c_para_begin(void *cx) { ccon *c = cx; c->col = 0; }
static void c_run(void *cx, const char *s, int n, int style) { hp_words(cx, s, n, (style & HS_BOLD) ? C_H1 : C_TEXT); }
static void c_link(void *cx, const char *href, int hn, const char *label, int ln)
{ (void)href; (void)hn; hp_words(cx, label, ln, C_LINK); }
static void c_para_end(void *cx) { ccon *c = cx; if (c->col) nl(); c->col = 0; }
static void c_code_begin(void *cx, int ex) { ccon *c = cx; (void)ex; if (c->col) nl(); c->col = 0; nl(); }
static void c_code_line(void *cx, const char *s, int n)
{ ccon *c = cx; setcol(C_CODE); puts0("  "); S->puts(s, n); setcol(C_TEXT); nl(); c->col = 0; }
static void c_code_end(void *cx) { (void)cx; }
static void c_rule(void *cx)
{ ccon *c = cx; if (c->col) nl(); nl(); for (int i = 0; i < c->width && i < 60; i++) S->putc('-'); nl(); c->col = 0; }

static char g_doc[32768];

static void show_topic(const char *topic)
{
    int n = help_load(topic, g_doc, sizeof g_doc);
    if (n < 0) {
        puts0("No help topic '"); puts0(topic); puts0("'.  Try  help INDEX\n");
        return;
    }
    ccon c;
    c.width = 78; c.col = 0;
    { int cols = 0, rows = 0; S->screen_size(&cols, &rows); if (cols > 4) c.width = cols - 2; }

    help_sink sk = {
        &c, c_title, c_heading, c_para_begin, c_run, c_link, c_para_end,
        c_code_begin, c_code_line, c_code_end, c_rule
    };
    help_parse(g_doc, &sk);
    setcol(C_TEXT); nl();
}

int main(int argc, char **argv)
{
    S = berry_svc;
    if (argc < 2 || !argv[1] || !argv[1][0]) show_topic("INDEX");
    else show_topic(argv[1]);
    return 0;
}
