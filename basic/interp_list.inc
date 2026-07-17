// ===========================================================================
// BerryBasiC — LIST: plain and pretty-printed (syntax colour + indentation)
//
// This file is a fragment of the interpreter and is #included by basic.c.
// It is NOT a standalone translation unit: it shares the single translation
// unit's static state and is compiled only as part of basic.c. Do not add it
// to the build system or compile it on its own.
// ===========================================================================
static void list_text(const char *t) {
    int i = 0;
    while (t[i]) {
        char c = t[i];
        if (c == '"') {                          // string literal: copy verbatim
            con_putc(c); i++;
            while (t[i] && t[i] != '"') con_putc(t[i++]);
            if (t[i] == '"') { con_putc('"'); i++; }
            continue;
        }
        if (!is_alpha(c)) { con_putc(c); i++; continue; }   // digits/punctuation/space

        // Gather a word (letters+digits, optional $ or % suffix) and a folded
        // copy to test against the keyword table.
        int j = i, wn = 0; char word[16];
        while (is_alnum(t[j])) { if (wn < 15) word[wn++] = up(t[j]); j++; }
        if ((t[j] == '$' || t[j] == '%') && wn < 15) word[wn++] = t[j], j++;
        word[wn] = 0;

        int is_kw = 0;
        for (int k = 0; k < kwcount; k++)
            if (s_eq(word, kwtab[k].name)) { is_kw = 1; break; }
        int is_proc = (word[0]=='P'&&word[1]=='R'&&word[2]=='O'&&word[3]=='C');
        int is_fn   = (word[0]=='F'&&word[1]=='N'&&wn>2);

        if (is_kw) {
            for (int p = i; p < j; p++) con_putc(up(t[p]));    // keyword -> UPPERCASE
            if (s_eq(word, "REM")) { i = j; while (t[i]) con_putc(t[i++]); break; }
        } else if (is_proc || is_fn) {                         // PROC/FN + name
            int plen = is_proc ? 4 : 2;
            for (int p = i; p < i + plen; p++) con_putc(up(t[p]));
            for (int p = i + plen; p < j; p++) con_putc(t[p]); // name as typed
        } else {
            for (int p = i; p < j; p++) con_putc(t[p]);        // variable as typed
        }
        i = j;
    }
}

static int word_is(const char *w);      // "LIST SIMPLE" / OFF-style bareword test

// --- pretty LIST: gutter, indentation and syntax colouring ------------------
// Colours are BBC logical indices (con_colour): on the framebuffer they tint the
// text, on the host CLI they map to ANSI, and in the unit tests they are a no-op.
#define LC_LINE  6      // line-number gutter  (cyan)
#define LC_KW    3      // keywords            (yellow)
#define LC_STR   2      // string literals     (green)
#define LC_NUM   5      // numeric literals    (magenta)
#define LC_NAME  6      // PROC/FN names       (cyan)
#define LC_REM   4      // REM comments        (blue)
#define LC_DEF   7      // everything else     (white)

static int is_hexd(char c) { c = up(c); return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); }
static int num_digits(int n) { int d = 1; if (n < 0) n = -n; while (n >= 10) { n /= 10; d++; } return d; }

// Analyse one program line for block indentation. Returns the net change in
// nesting depth the line makes (openers +1, closers -1), and sets *dedent_first
// when the line *begins* with a closer or a mid-block keyword (NEXT, ENDIF,
// ELSE, ...) so the line itself renders one level out. A multi-line IF is one
// whose last word is THEN; a one-line DEF (a classic FN= or a PROC:...:ENDPROC)
// nets to zero, so only true multi-line definitions open a block.
static int line_blocks(const char *t, int *dedent_first) {
    *dedent_first = 0;
    int delta = 0, seen = 0, defs = 0, has_eq = 0;
    char last[16]; last[0] = 0;
    int i = 0;
    while (t[i]) {
        if (t[i] == '"') { i++; while (t[i] && t[i] != '"') i++; if (t[i]) i++; continue; }
        if (t[i] == '=') { has_eq = 1; i++; continue; }
        if (!is_alpha(t[i])) { i++; continue; }
        char w[16]; int wn = 0;
        while (is_alnum(t[i])) { if (wn < 15) w[wn++] = up(t[i]); i++; }
        w[wn] = 0;
        s_copy(last, w, 16);
        int open = 0, close = 0, mid = 0;
        if (s_eq(w, "FOR") || s_eq(w, "REPEAT") || s_eq(w, "WHILE") ||
            s_eq(w, "CASE") || s_eq(w, "TRY") || s_eq(w, "TYPE")) open = 1;
        else if (s_eq(w, "DEF")) defs++;                       // resolved after has_eq is known
        else if (s_eq(w, "NEXT") || s_eq(w, "UNTIL") || s_eq(w, "ENDWHILE") ||
                 s_eq(w, "ENDCASE") || s_eq(w, "ENDTRY") || s_eq(w, "ENDPROC") ||
                 s_eq(w, "ENDIF") || s_eq(w, "ENDTYPE")) close = 1;
        else if (s_eq(w, "ELSE") || s_eq(w, "CATCH")) mid = 1;
        else if (s_eq(w, "END")) {                             // END PROC / END FN close a block
            int k = i; while (t[k] == ' ') k++;
            char nw[8]; int nn = 0;
            while (is_alpha(t[k]) && nn < 7) { nw[nn++] = up(t[k]); k++; }
            nw[nn] = 0;
            if (s_eq(nw, "PROC") || s_eq(nw, "FN")) close = 1;
        }
        if (open)  delta++;
        if (close) delta--;
        if (!seen && (close || mid)) *dedent_first = 1;
        seen = 1;
    }
    if (!has_eq) delta += defs;                                // a real multi-line DEF opens
    if (s_eq(last, "THEN")) delta++;                           // block IF ... THEN
    return delta;
}

// Render a program line with syntax colouring: keywords upper-cased and coloured,
// strings, numbers, REM comments and PROC/FN names each in their own colour, the
// rest (variables, operators) in the default. Mirrors list_text's word handling.
static void list_fancy_text(const char *t) {
    int i = 0;
    while (t[i]) {
        char c = t[i];
        if (c == '"') {                                        // string literal
            con_colour(LC_STR);
            con_putc(c); i++;
            while (t[i] && t[i] != '"') con_putc(t[i++]);
            if (t[i] == '"') { con_putc('"'); i++; }
            con_colour(LC_DEF);
            continue;
        }
        if (c == '&') {                                        // hex literal &FF
            con_colour(LC_NUM); con_putc('&'); i++;
            while (is_hexd(t[i])) con_putc(t[i++]);
            con_colour(LC_DEF);
            continue;
        }
        if (c == '%' && (t[i + 1] == '0' || t[i + 1] == '1')) {  // binary literal %1010
            con_colour(LC_NUM); con_putc('%'); i++;
            while (t[i] == '0' || t[i] == '1') con_putc(t[i++]);
            con_colour(LC_DEF);
            continue;
        }
        if (is_digit(c) || (c == '.' && is_digit(t[i + 1]))) {   // decimal / E-notation
            con_colour(LC_NUM);
            while (is_digit(t[i])) con_putc(t[i++]);
            if (t[i] == '.') { con_putc(t[i++]); while (is_digit(t[i])) con_putc(t[i++]); }
            if ((t[i] == 'E' || t[i] == 'e') &&
                (is_digit(t[i + 1]) || ((t[i + 1] == '+' || t[i + 1] == '-') && is_digit(t[i + 2])))) {
                con_putc(t[i++]);
                if (t[i] == '+' || t[i] == '-') con_putc(t[i++]);
                while (is_digit(t[i])) con_putc(t[i++]);
            }
            con_colour(LC_DEF);
            continue;
        }
        if (!is_alpha(c)) { con_putc(c); i++; continue; }       // operators / punctuation

        int j = i, wn = 0; char word[16];
        while (is_alnum(t[j])) { if (wn < 15) word[wn++] = up(t[j]); j++; }
        if ((t[j] == '$' || t[j] == '%') && wn < 15) word[wn++] = t[j], j++;
        word[wn] = 0;

        int is_kw = 0;
        for (int k = 0; k < kwcount; k++)
            if (s_eq(word, kwtab[k].name)) { is_kw = 1; break; }
        int is_proc = (word[0] == 'P' && word[1] == 'R' && word[2] == 'O' && word[3] == 'C');
        int is_fn   = (word[0] == 'F' && word[1] == 'N' && wn > 2);

        if (is_kw) {
            con_colour(LC_KW);
            for (int p = i; p < j; p++) con_putc(up(t[p]));     // keyword -> UPPERCASE
            con_colour(LC_DEF);
            if (s_eq(word, "REM")) {                            // colour the comment tail
                con_colour(LC_REM);
                i = j; while (t[i]) con_putc(t[i++]);
                con_colour(LC_DEF);
                break;
            }
        } else if (is_proc || is_fn) {                         // PROC/FN + name
            int plen = is_proc ? 4 : 2;
            con_colour(LC_KW);
            for (int p = i; p < i + plen; p++) con_putc(up(t[p]));
            con_colour(LC_NAME);
            for (int p = i + plen; p < j; p++) con_putc(t[p]);  // name as typed
            con_colour(LC_DEF);
        } else {
            for (int p = i; p < j; p++) con_putc(t[p]);         // variable as typed
        }
        i = j;
    }
}

static void list_file(const char *name, int simple);   // LIST "file" (defined with storage)

// LIST [SIMPLE] [start][,end] : whole program, a single line (LIST 100), a range
// (LIST 100,200), from a line (LIST 100,) or up to a line (LIST ,200). By default
// the listing is pretty-printed - line numbers right-aligned in a gutter sized to
// the largest number, structural indentation, and syntax colouring. LIST SIMPLE
// gives the plain "number space text" form (e.g. for copying or a mono terminal).
// LIST [SIMPLE] "file" lists a program file on the card instead of the one in
// memory (same pretty/plain forms), with screen paging.
static void stmt_list(void) {
    lex_next();
    int simple = 0;
    if (word_is("SIMPLE")) { simple = 1; lex_next(); }
    if (tok == T_STR) {                                             // LIST [SIMPLE] "file"
        char name[64];
        int k = 0;
        while (tok_str[k] && k < (int)sizeof(name) - 5) { name[k] = tok_str[k]; k++; }
        name[k] = 0;
        lex_next();
        int hasdot = 0;                                            // default the extension to .BAS
        for (int j = 0; name[j]; j++) if (name[j] == '.') hasdot = 1;
        if (!hasdot) { name[k++] = '.'; name[k++] = 'B'; name[k++] = 'A'; name[k++] = 'S'; name[k] = 0; }
        list_file(name, simple);
        return;
    }
    int lo = 0, hi = 0x7FFFFFFF;
    if (tok == T_NUM) { lo = (int)tok_num; hi = lo; lex_next(); }   // single line by default
    if (tok == T_COMMA) {                                           // a range was requested
        lex_next();
        hi = 0x7FFFFFFF;
        if (tok == T_NUM) { hi = (int)tok_num; lex_next(); }
    }

    if (simple) {
        for (int i = 0; i < prog_n; i++) {
            if (prog[i].module != 0) continue;                     // never list module lines
            if (prog[i].num < lo || prog[i].num > hi) continue;
            con_putn(prog[i].num);
            con_putc(' ');
            list_text(prog[i].text);
            con_putc('\n');
        }
        return;
    }

    // Pretty form. First find the widest line number among the lines we'll print,
    // so the gutter is exactly as wide as it needs to be.
    int maxnum = 0;
    for (int i = 0; i < prog_n; i++) {
        if (prog[i].module != 0) continue;
        if (prog[i].num < lo || prog[i].num > hi) continue;
        if (prog[i].num > maxnum) maxnum = prog[i].num;
    }
    int gutter = num_digits(maxnum);

    // Walk every main line to track block nesting (so a partial listing is still
    // correctly indented), printing the ones inside the range.
    int indent = 0;
    for (int i = 0; i < prog_n; i++) {
        if (prog[i].module != 0) continue;
        int dedent_first;
        int delta = line_blocks(prog[i].text, &dedent_first);
        int render = indent - (dedent_first ? 1 : 0);
        if (render < 0) render = 0;

        if (prog[i].num >= lo && prog[i].num <= hi) {
            for (int k = num_digits(prog[i].num); k < gutter; k++) con_putc(' ');  // right-align
            con_colour(LC_LINE);
            con_putn(prog[i].num);
            con_colour(LC_DEF);
            con_putc(' ');
            for (int k = 0; k < render * 2; k++) con_putc(' ');                    // indentation
            list_fancy_text(prog[i].text);
            con_putc('\n');
        }
        indent += delta;
        if (indent < 0) indent = 0;
    }
    con_colour(LC_DEF);                                            // leave text back to normal
}

