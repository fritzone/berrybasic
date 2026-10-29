/* ===========================================================================
 *  helpfmt.h - the BerryBasiC help-document format, shared by HELP.POD (the
 *  console printer) and HELPVIEW.POD (the graphical browser).
 *
 *  A help file is a small, well-formed subset of HTML. Block elements sit on
 *  their own lines; inline markup appears inside <p> prose. The parser walks a
 *  document and drives a "sink" of callbacks, so each front-end only decides how
 *  to render - not how to parse.
 *
 *  Supported markup
 *  ----------------
 *    <title>NAME</title>          the topic's title (metadata)
 *    <h1>...</h1>  <h2>...</h2>    headings
 *    <p> ... </p>                 a paragraph of prose (word-wrapped by the sink)
 *    <pre> ... </pre>             verbatim block (kept as-is)
 *    <code> ... </code>           an example (verbatim; the browser syntax-colours it)
 *    <hr>                         a horizontal rule
 *  Inside <p>: plain text, <b>bold</b>, <i>italic</i>, and
 *    <a href="TOPIC">label</a>    a link to another help file (TOPIC.HTML)
 *  Entities &lt; &gt; &amp; &quot; are decoded in prose. <pre>/<code> bodies are
 *  raw (read literally up to the closing tag), so BASIC examples need no escaping.
 * ======================================================================== */
#ifndef HELPFMT_H
#define HELPFMT_H

#include <stdio.h>
#include <string.h>

/* Inline style bits passed to sink->run. */
#define HS_BOLD   1
#define HS_ITALIC 2

typedef struct help_sink {
    void *ctx;
    void (*title)(void *ctx, const char *s, int n);
    void (*heading)(void *ctx, int level, const char *s, int n);   /* 1 or 2 */
    void (*para_begin)(void *ctx);
    void (*run)(void *ctx, const char *s, int n, int style);       /* inline text */
    void (*link)(void *ctx, const char *href, int hn,
                 const char *label, int ln);                       /* <a href> */
    void (*para_end)(void *ctx);
    void (*code_begin)(void *ctx, int is_example);                 /* <code>=1, <pre>=0 */
    void (*code_line)(void *ctx, const char *s, int n);
    void (*code_end)(void *ctx);
    void (*rule)(void *ctx);
} help_sink;

/* --- load a help file for `topic` into buf; returns length, or -1 --------- */
static int help_upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

/* Build "/SYS/HELP/BASIC/<TOPIC>.HTML" (topic upper-cased) into `path`. */
static void help_path(const char *topic, char *path, int sz)
{
    const char *pre = "/SYS/HELP/BASIC/";
    int i = 0, j = 0;
    for (; pre[j] && i < sz - 1; j++) path[i++] = pre[j];
    for (j = 0; topic[j] && i < sz - 6; j++) path[i++] = (char)help_upper((unsigned char)topic[j]);
    const char *ext = ".HTML";
    for (j = 0; ext[j] && i < sz - 1; j++) path[i++] = ext[j];
    path[i] = 0;
}

static int help_load(const char *topic, char *buf, int bufsz)
{
    char path[96];
    help_path(topic, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int n = (int)fread(buf, 1, bufsz - 1, f);
    fclose(f);
    if (n < 0) n = 0;
    buf[n] = 0;
    return n;
}

/* --- the parser ----------------------------------------------------------- */

/* Case-insensitive match of the tag opening at p ("<h1", "</p", ...). Returns
 * the length matched (past the '>'), 0 if no match. `open` is e.g. "<h1>". */
static int hf_is(const char *p, const char *tag)
{
    int i = 0;
    for (; tag[i]; i++) if (help_upper((unsigned char)p[i]) != help_upper((unsigned char)tag[i])) return 0;
    return i;
}

/* Copy raw bytes from p until the literal close tag, emitting each line via
 * code_line. Returns a pointer just past the close tag. */
static const char *hf_verbatim(const char *p, const char *close, const help_sink *sk)
{
    if (*p == '\r') p++;
    if (*p == '\n') p++;                     /* skip the newline right after <code>/<pre> */
    for (;;) {
        /* find end of this line or the close tag */
        const char *ls = p;
        while (*p && *p != '\n' && !hf_is(p, close)) p++;
        int len = (int)(p - ls);
        /* trim a trailing CR */
        if (len > 0 && ls[len - 1] == '\r') len--;
        if (hf_is(p, close)) {
            if (len > 0) sk->code_line(sk->ctx, ls, len);
            return p + (int)strlen(close);
        }
        sk->code_line(sk->ctx, ls, len);
        if (*p == '\n') p++;
        if (!*p) return p;
    }
}

/* Decode entities in a prose run into `out` (up to n). Returns length. */
static int hf_decode(const char *s, int len, char *out, int outmax)
{
    int o = 0;
    for (int i = 0; i < len && o < outmax - 1; i++) {
        if (s[i] == '&') {
            if (hf_is(s + i, "&lt;"))       { out[o++] = '<'; i += 3; continue; }
            if (hf_is(s + i, "&gt;"))       { out[o++] = '>'; i += 3; continue; }
            if (hf_is(s + i, "&amp;"))      { out[o++] = '&'; i += 4; continue; }
            if (hf_is(s + i, "&quot;"))     { out[o++] = '"'; i += 5; continue; }
        }
        out[o++] = s[i];
    }
    out[o] = 0;
    return o;
}

/* Emit a prose run (with entity decoding) via sink->run. */
static void hf_run(const char *s, int len, int style, const help_sink *sk)
{
    static char tmp[512];
    while (len > 0) {
        int chunk = len < (int)sizeof(tmp) - 1 ? len : (int)sizeof(tmp) - 1;
        /* don't split an entity across chunks: back up to a safe boundary */
        int n = hf_decode(s, chunk, tmp, sizeof tmp);
        if (n > 0) sk->run(sk->ctx, tmp, n, style);
        s += chunk; len -= chunk;
    }
}

/* Parse inline content of a <p> until </p>. */
static const char *hf_inline(const char *p, const help_sink *sk)
{
    const char *run0 = p;
    int style = 0;
    for (;;) {
        if (!*p || hf_is(p, "</p>")) {
            if (p > run0) hf_run(run0, (int)(p - run0), style, sk);
            return *p ? p + 4 : p;
        }
        if (*p == '<') {
            /* flush the text run before the tag */
            if (p > run0) hf_run(run0, (int)(p - run0), style, sk);
            int L;
            if ((L = hf_is(p, "<b>")))  { style |= HS_BOLD;   p += L; run0 = p; continue; }
            if ((L = hf_is(p, "</b>"))) { style &= ~HS_BOLD;  p += L; run0 = p; continue; }
            if ((L = hf_is(p, "<i>")))  { style |= HS_ITALIC; p += L; run0 = p; continue; }
            if ((L = hf_is(p, "</i>"))) { style &= ~HS_ITALIC;p += L; run0 = p; continue; }
            if (hf_is(p, "<a ")) {
                /* <a href="TOPIC">label</a> */
                const char *h = strstr(p, "href=");
                const char *href = ""; int hn = 0;
                if (h && h < strchr(p, '>')) {
                    h += 5; if (*h == '"' || *h == '\'') h++;
                    const char *he = h;
                    while (*he && *he != '"' && *he != '\'' && *he != '>') he++;
                    href = h; hn = (int)(he - h);
                }
                const char *gt = strchr(p, '>');
                if (!gt) { p++; run0 = p; continue; }
                const char *lab = gt + 1;
                const char *end = strstr(lab, "</a>");
                if (!end) end = lab;
                sk->link(sk->ctx, href, hn, lab, (int)(end - lab));
                p = *end ? end + 4 : end;
                run0 = p; continue;
            }
            /* unknown tag: skip to '>' */
            const char *gt = strchr(p, '>');
            p = gt ? gt + 1 : p + 1;
            run0 = p; continue;
        }
        p++;
    }
}

static void help_parse(const char *doc, const help_sink *sk)
{
    const char *p = doc;
    while (*p) {
        if (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') { p++; continue; }
        if (*p == '<') {
            int L;
            if ((L = hf_is(p, "<title>"))) {
                const char *s = p + L, *e = strstr(s, "</title>");
                if (!e) e = s + strlen(s);
                if (sk->title) sk->title(sk->ctx, s, (int)(e - s));
                p = *e ? e + 8 : e; continue;
            }
            if ((L = hf_is(p, "<h1>"))) {
                const char *s = p + L, *e = strstr(s, "</h1>");
                if (!e) e = s + strlen(s);
                sk->heading(sk->ctx, 1, s, (int)(e - s));
                p = *e ? e + 5 : e; continue;
            }
            if ((L = hf_is(p, "<h2>"))) {
                const char *s = p + L, *e = strstr(s, "</h2>");
                if (!e) e = s + strlen(s);
                sk->heading(sk->ctx, 2, s, (int)(e - s));
                p = *e ? e + 5 : e; continue;
            }
            if ((L = hf_is(p, "<pre>")))  { sk->code_begin(sk->ctx, 0); p = hf_verbatim(p + L, "</pre>",  sk); sk->code_end(sk->ctx); continue; }
            if ((L = hf_is(p, "<code>"))) { sk->code_begin(sk->ctx, 1); p = hf_verbatim(p + L, "</code>", sk); sk->code_end(sk->ctx); continue; }
            if (hf_is(p, "<hr")) { sk->rule(sk->ctx); const char *gt = strchr(p, '>'); p = gt ? gt + 1 : p + 3; continue; }
            if ((L = hf_is(p, "<p>"))) { sk->para_begin(sk->ctx); p = hf_inline(p + L, sk); sk->para_end(sk->ctx); continue; }
            /* unknown block tag: skip it */
            const char *gt = strchr(p, '>'); p = gt ? gt + 1 : p + 1; continue;
        }
        /* bare text at block level -> treat as a paragraph */
        sk->para_begin(sk->ctx);
        p = hf_inline(p, sk);
        sk->para_end(sk->ctx);
    }
}

#endif /* HELPFMT_H */
