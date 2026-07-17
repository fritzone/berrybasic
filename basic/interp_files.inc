// ===========================================================================
// BerryBasiC — storage commands: LOAD/SAVE, modules (IMPORT), CAT, dir ops, editor
//
// This file is a fragment of the interpreter and is #included by basic.c.
// It is NOT a standalone translation unit: it shares the single translation
// unit's static state and is compiled only as part of basic.c. Do not add it
// to the build system or compile it on its own.
// ===========================================================================
// --- Storage statements (LOAD / SAVE / CAT / DELETE) -------------------------
#define STG_BUF_SIZE 65536
static char stg_buf[STG_BUF_SIZE];

// Map a storage error code to a BASIC error message.
static void stg_err(int code) {
    switch (code) {
        case STG_ENOTFOUND: err("File not found");                 break;
        case STG_EEXIST:    err("Already exists");                 break;
        case STG_ENOTEMPTY: err("Directory not empty");            break;
        case STG_EFULL:     err("Storage card is full");           break;
        case STG_ENOFS:     err("No storage card found");          break;
        case STG_ETOOBIG:   err("File is too big to load");        break;
        default:            err("Storage read/write error");       break;
    }
}

// Read the next filename operand into a NUL-terminated buffer. Accepts either a
// quoted string ("name.bas") or a bare filename typed without quotes (name.bas
// or just name). Reads straight from the raw source so characters like '.' don't
// trip the expression tokeniser, then resyncs the tokeniser. If no extension is
// given, ".BAS" is appended, so "LOAD WELCOME" finds WELCOME.BAS. The FAT layer
// is case-insensitive, so the name is kept as typed. Callers must NOT lex past
// the keyword first - lx still points just after LOAD/SAVE/DELETE.
static int read_filename(char *out, int outsz) {
    while (is_space(*lx)) lx++;
    int n = 0;
    if (*lx == '"') {                          // quoted form: "name"
        lx++;
        while (*lx && *lx != '"') { if (n < outsz - 1) out[n++] = *lx; lx++; }
        if (*lx == '"') lx++;
    } else {                                   // bare form: letters digits . / _ -
        while (*lx && (is_alnum(*lx) || *lx == '.' || *lx == '/' ||
                       *lx == '_' || *lx == '-')) {
            if (n < outsz - 1) out[n++] = *lx;
            lx++;
        }
    }
    out[n] = 0;
    if (n == 0) { err("Expected a file name"); return 0; }

    // Default the extension to .BAS when the name has none.
    int has_dot = 0;
    for (int i = 0; i < n; i++) if (out[i] == '.') has_dot = 1;
    if (!has_dot && n + 4 < outsz) {
        out[n++] = '.'; out[n++] = 'B'; out[n++] = 'A'; out[n++] = 'S';
        out[n] = 0;
    }

    lex_next();                                // resync the tokeniser for what follows
    return 1;
}

static int uint_to_str(char *p, int v) {        // unsigned decimal -> p, returns length
    char t[12]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) p[i] = t[n - 1 - i];
    return n;
}

// SAVE "name" : serialise the program ("<num> <text>\n" per line) and write it.
static void stmt_save(void) {
    char name[64];
    if (!read_filename(name, sizeof name)) return;
    int pos = 0;
    for (int i = 0; i < prog_n && pos < STG_BUF_SIZE - LINE_LEN - 16; i++) {
        if (prog[i].module != 0) continue;                         // never save module lines
        pos += uint_to_str(stg_buf + pos, prog[i].num);
        stg_buf[pos++] = ' ';
        const char *t = prog[i].text;
        while (*t && pos < STG_BUF_SIZE - 2) stg_buf[pos++] = *t++;
        stg_buf[pos++] = '\n';
    }
    int r = stg_write(name, stg_buf, pos);
    if (r) stg_err(r);
}

// LOAD "name" : clear the current program and parse the file's numbered lines.
static void stmt_load(void) {
    char name[64];
    if (!read_filename(name, sizeof name)) return;
    int n = stg_read(name, stg_buf, STG_BUF_SIZE);
    if (n < 0) { stg_err(n); return; }

    prog_n = 0;
    clear_vars();
    int i = 0;
    while (i < n) {
        char line[LINE_LEN];
        int j = 0;
        while (i < n && stg_buf[i] != '\n' && stg_buf[i] != '\r') {
            if (j < LINE_LEN - 1) line[j++] = stg_buf[i];
            i++;
        }
        line[j] = 0;
        while (i < n && (stg_buf[i] == '\n' || stg_buf[i] == '\r')) i++;
        char *p = line;
        while (is_space(*p)) p++;
        if (is_digit(*p)) {
            int num = 0;
            while (is_digit(*p)) { num = num * 10 + (*p - '0'); p++; }
            while (is_space(*p)) p++;
            prog_store(num, p);
        }
    }
}

// LIST "file" support (the parser is in interp_list.inc). Splits a raw file line
// "NUM TEXT" into *num and a pointer to TEXT (past the number and one space);
// returns the whole line with *num = 0 when there is no leading number.
static const char *list_line_split(const char *line, int *num) {
    const char *p = line;
    while (is_space(*p)) p++;
    if (!is_digit(*p)) { *num = 0; return line; }
    int v = 0;
    while (is_digit(*p)) { v = v * 10 + (*p - '0'); p++; }
    *num = v;
    if (is_space(*p)) p++;                         // the single separating space
    return p;
}

// Pause the listing when the screen fills (target only; con_rows()==0 on host and
// in tests, so page==0 and nothing ever blocks). Returns 1 if the user quit.
static int list_page(int printed, int page) {
    if (!page || printed % page != 0) return 0;
    con_colour(LC_DEF);
    con_puts("-- more (any key, Q quits) --");
    int k = con_getkey();
    con_putc('\r');                                // erase the prompt
    for (int s = 0; s < 30; s++) con_putc(' ');
    con_putc('\r');
    return (k == 'q' || k == 'Q' || k == 27);
}

// LIST [SIMPLE] "file": list a program file from the card. Pretty form mirrors
// the in-memory LIST (gutter-aligned numbers, block indentation, syntax colour);
// SIMPLE prints the file's lines verbatim. Both page when longer than the screen.
//
// Copy the next '\n'/'\r'-terminated line of buf[*i..n) into out (NUL-terminated,
// clamped to LINE_LEN), advancing *i past the line and its terminator. Returns 0
// at end of buffer.
static int list_next_line(const char *buf, int n, int *i, char *out) {
    if (*i >= n) return 0;
    int j = 0;
    while (*i < n && buf[*i] != '\n' && buf[*i] != '\r') {
        if (j < LINE_LEN - 1) out[j++] = buf[*i];
        (*i)++;
    }
    out[j] = 0;
    while (*i < n && (buf[*i] == '\n' || buf[*i] == '\r')) (*i)++;
    return 1;
}

static void list_file(const char *name, int simple) {
    int n = stg_read(name, stg_buf, STG_BUF_SIZE);
    if (n < 0) { stg_err(n); return; }

    int rows = con_rows();
    int page = rows > 1 ? rows - 1 : 0;
    char line[LINE_LEN];

    if (simple) {
        int printed = 0, i = 0;
        while (list_next_line(stg_buf, n, &i, line)) {
            con_puts(line); con_putc('\n');
            if (list_page(++printed, page)) return;
        }
        return;
    }

    // Pass 1: the widest line number, to size the gutter.
    int maxnum = 0, i = 0;
    while (list_next_line(stg_buf, n, &i, line)) {
        int num; list_line_split(line, &num);
        if (num > maxnum) maxnum = num;
    }
    int gutter = num_digits(maxnum);

    // Pass 2: pretty-print, tracking block nesting for indentation.
    int printed = 0, indent = 0;
    i = 0;
    while (list_next_line(stg_buf, n, &i, line)) {
        int num; const char *text = list_line_split(line, &num);
        int dedent_first;
        int delta = line_blocks(text, &dedent_first);
        int render = indent - (dedent_first ? 1 : 0);
        if (render < 0) render = 0;

        for (int k = num_digits(num); k < gutter; k++) con_putc(' ');   // right-align
        con_colour(LC_LINE); con_putn(num);
        con_colour(LC_DEF);  con_putc(' ');
        for (int k = 0; k < render * 2; k++) con_putc(' ');             // indentation
        list_fancy_text(text);
        con_putc('\n');

        indent += delta; if (indent < 0) indent = 0;
        if (list_page(++printed, page)) { con_colour(LC_DEF); return; }
    }
    con_colour(LC_DEF);
}

// ---------------------------------------------------------------------------
// Modules (IMPORT). A module is an ordinary numbered BASIC text file holding
// DEF PROC / DEF fn definitions. At RUN, every `IMPORT "name"` line in the main
// program (and, transitively, in imported modules) loads that file and appends
// its lines to prog[] under a fresh module id. Line numbers may overlap the main
// program's because line-number lookups are scoped per module (see cur_module).
// The appended lines are stripped when the run ends, so editing/LIST/SAVE only
// ever see the main program.
// ---------------------------------------------------------------------------
#define MAX_MODULES 16
static char imported_names[MAX_MODULES][64];
static int  n_imported;

// Append one line to `module`'s block, keeping that block sorted by line number.
// Module blocks sit at the tail of prog[] (after main and earlier modules).
static void module_add_line(int module, int num, const char *text) {
    if (prog_n >= MAX_LINES) { err("Out of memory"); return; }
    int i = prog_n;
    while (i > 0 && prog[i-1].module == module && prog[i-1].num > num) {
        prog[i] = prog[i-1]; i--;
    }
    prog[i].num = num; prog[i].module = module;
    s_copy(prog[i].text, text, LINE_LEN);
    prog_n++;
}

// Load a module file and append its numbered lines under `module`. Returns 0, or
// a negative STG_* error if the file could not be read.
static int load_module(const char *name, int module) {
    int n = stg_read(name, stg_buf, STG_BUF_SIZE);
    if (n < 0) return n;
    int i = 0;
    while (i < n && !g_err) {
        char line[LINE_LEN];
        int j = 0;
        while (i < n && stg_buf[i] != '\n' && stg_buf[i] != '\r') {
            if (j < LINE_LEN - 1) line[j++] = stg_buf[i];
            i++;
        }
        line[j] = 0;
        while (i < n && (stg_buf[i] == '\n' || stg_buf[i] == '\r')) i++;
        char *p = line;
        while (is_space(*p)) p++;
        if (is_digit(*p)) {
            int num = 0;
            while (is_digit(*p)) { num = num * 10 + (*p - '0'); p++; }
            while (is_space(*p)) p++;
            module_add_line(module, num, p);
        }
    }
    return 0;
}

// If `text` begins with IMPORT "name", copy the (.BAS-defaulted) file name into
// `out` and return 1; otherwise 0. Uses the global lexer, safe in the pre-pass.
static int line_import_name(const char *text, char *out, int outsz) {
    cur_text = text; lx = text; lex_next();
    if (!(tok == T_KW && tok_kw == KW_IMPORT)) return 0;
    return read_filename(out, outsz);          // reads from lx, applies the .BAS default
}

// Pre-pass, run before a program executes: pull in every module reachable via
// IMPORT and append its lines. prog_n grows as modules are added, so the loop
// also visits IMPORT lines inside modules (transitive). Dedup by name breaks
// cycles and avoids importing the same module twice.
static void import_modules(void) {
    n_imported = 0;
    for (int i = 0; i < prog_n && !g_err; i++) {
        char name[64];
        if (!line_import_name(prog[i].text, name, sizeof name)) {
            if (g_err) return;                 // malformed IMPORT (no name)
            continue;
        }
        int dup = 0;
        for (int k = 0; k < n_imported; k++)
            if (s_eq(imported_names[k], name)) { dup = 1; break; }
        if (dup) continue;
        if (n_imported >= MAX_MODULES) { err("Too many imported modules"); return; }
        s_copy(imported_names[n_imported], name, 64);
        if (load_module(name, n_imported + 1) < 0) { err("Imported module not found"); return; }
        n_imported++;
    }
}

static void stmt_delete(void) {
    char name[64];
    if (!read_filename(name, sizeof name)) return;
    int r = stg_delete(name);
    if (r) stg_err(r);
}

// --- CAT: a rich directory listing ------------------------------------------
// Categorise a file by extension into a short icon tag and a colour (BBC index).
static void cat_kind(const char *name, int is_dir, const char **icon, int *colour) {
    if (is_dir) { *icon = "[DIR]"; *colour = 4; return; }             // blue
    int n = 0; while (name[n]) n++;
    int dot = -1;
    for (int i = n - 1; i >= 0; i--) { if (name[i] == '.') { dot = i; break; } if (name[i] == '/') break; }
    char ext[8]; int el = 0;
    if (dot >= 0) for (int i = dot + 1; name[i] && el < 7; i++) ext[el++] = up(name[i]);
    ext[el] = 0;
    if (s_eq(ext, "BAS")) { *icon = "[BAS]"; *colour = 3; return; }   // program  - yellow
    if (s_eq(ext, "SED")) { *icon = "[SED]"; *colour = 5; return; }   // seed     - magenta
    if (s_eq(ext, "PNG") || s_eq(ext, "JPG") || s_eq(ext, "JPEG") ||
        s_eq(ext, "BMP") || s_eq(ext, "GIF"))
                          { *icon = "[IMG]"; *colour = 2; return; }   // image    - green
    if (s_eq(ext, "TTF") || s_eq(ext, "TTC") || s_eq(ext, "OTF"))
                          { *icon = "[FNT]"; *colour = 6; return; }   // font     - cyan
    if (s_eq(ext, "TXT") || s_eq(ext, "MD") || s_eq(ext, "LOG") ||
        s_eq(ext, "INI") || s_eq(ext, "CFG"))
                          { *icon = "[TXT]"; *colour = 7; return; }   // text     - white
    *icon = "[BIN]"; *colour = 7;                                     // anything else
}

static int cat_u_to_str(char *out, long v) {          // non-negative -> decimal, returns len
    char t[24]; int n = 0;
    if (v <= 0) { out[0] = '0'; out[1] = 0; return 1; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = 0; return n;
}

// Human-readable size: "845", "12.3K", "4.0M".
static void cat_size_str(long sz, char *out) {
    if (sz < 0) sz = 0;
    if (sz < 1024) { cat_u_to_str(out, sz); return; }
    long div  = (sz < 1024L * 1024) ? 1024L : 1024L * 1024L;
    char unit = (sz < 1024L * 1024) ? 'K' : 'M';
    long tenths = sz * 10 / div;
    int n = cat_u_to_str(out, tenths / 10);
    out[n++] = '.'; out[n++] = (char)('0' + tenths % 10); out[n++] = unit; out[n] = 0;
}

static void cat_pad(int n) { while (n-- > 0) con_putc(' '); }
static void cat_2d(int v)  { con_putc((char)('0' + (v / 10) % 10)); con_putc((char)('0' + v % 10)); }

// CAT / DIR : list the current directory. By default it is rich - a coloured
// type icon per file, the name, a human-readable size, and the date/time, in
// aligned columns, pausing for a keypress when the screen fills. CAT SIMPLE gives
// the plain one-name-per-line listing (and is what a program should use).
// Plain one-name-per-line listing of a directory (CAT SIMPLE "path"). Same shape
// as the backend stg_dir(), but works for any directory through the path-aware
// scan cursor rather than only the current one.
static void cat_simple(const char *path) {
    int r = stg_diropen(path);
    if (r != 0) { stg_err(r); return; }
    while (stg_dirnext(&g_dirent) == 1) {
        con_puts(g_dirent.name);
        if (g_dirent.is_dir) con_puts("  <DIR>");
        con_putc('\n');
    }
}

// CAT [SIMPLE] ["path"] : list a directory (the current one by default). The rich
// form shows coloured type icons, sizes and dates with screen paging; SIMPLE
// prints just the names. An optional path lets you peek into a subdirectory
// (e.g. CAT "SEED") without CD-ing into it.
static void stmt_cat(void) {
    lex_next();                                       // consume CAT / DIR
    int simple = 0;
    if (word_is("SIMPLE")) { simple = 1; lex_next(); }

    char pathbuf[80];
    const char *path = ".";
    if (tok != T_EOL && tok != T_COLON) {             // optional directory to list
        value_t s = need_str(); if (g_err) return;
        copy_fname(s, pathbuf, sizeof pathbuf);
        path = pathbuf;
    }

    if (simple) {
        // No path = the current directory: keep the exact backend listing.
        if (path[0] == '.' && path[1] == 0) stg_dir();
        else                                cat_simple(path);
        return;
    }

    // Pass 1: the widest name (to size the name column) and the entry count.
    int r = stg_diropen(path);
    if (r != 0) { stg_err(r); return; }
    int maxname = 0, count = 0;
    while (stg_dirnext(&g_dirent) == 1) {
        int n = 0; while (g_dirent.name[n]) n++;
        if (n > maxname) maxname = n;
        count++;
    }
    if (count == 0) { con_puts("(empty)\n"); return; }
    int namew = maxname; if (namew < 12) namew = 12; if (namew > 28) namew = 28;

    int rows = con_rows();
    int page = rows > 1 ? rows - 1 : 0;               // 0 = do not page (host/tests)
    int printed = 0, files = 0, dirs = 0;
    long total = 0;

    // Pass 2: print each entry.
    stg_diropen(path);
    while (stg_dirnext(&g_dirent) == 1) {
        const char *icon; int colour;
        cat_kind(g_dirent.name, g_dirent.is_dir, &icon, &colour);
        int nl = 0; while (g_dirent.name[nl]) nl++;

        con_colour(colour);
        con_puts(icon); con_putc(' ');                // [BAS] etc.
        con_puts(g_dirent.name);                      // name (same colour)
        con_colour(7);
        cat_pad(namew - nl + 2);

        if (g_dirent.is_dir) { cat_pad(6); con_puts("-"); dirs++; }   // size column
        else {
            char sz[24]; cat_size_str(g_dirent.size, sz);
            int sl = 0; while (sz[sl]) sl++;
            cat_pad(7 - sl); con_puts(sz);
            total += g_dirent.size; files++;
        }

        con_puts("  ");                               // date  time
        if (g_dirent.year > 0) {
            char y[8]; int yl = cat_u_to_str(y, g_dirent.year);
            cat_pad(4 - yl); con_puts(y);
            con_putc('-'); cat_2d(g_dirent.month); con_putc('-'); cat_2d(g_dirent.day);
            con_putc(' '); cat_2d(g_dirent.hour);  con_putc(':'); cat_2d(g_dirent.minute);
        }
        con_putc('\n');

        printed++;
        if (page && printed % page == 0) {            // screen full: wait for a key
            con_puts("-- more (any key, Q quits) --");
            int k = con_getkey();
            con_putc('\r'); cat_pad(30); con_putc('\r');   // erase the prompt
            if (k == 'q' || k == 'Q' || k == 27) { con_colour(7); return; }
        }
    }

    con_colour(6);                                    // footer summary
    con_putn(files); con_puts(files == 1 ? " file, " : " files, ");
    con_putn(dirs);  con_puts(dirs == 1 ? " dir, "  : " dirs, ");
    { char sz[24]; cat_size_str(total, sz); con_puts(sz); con_puts(" total\n"); }
    con_colour(7);
}

// Directory commands take a quoted path (no ".BAS" default): MKDIR/CD/RMDIR
// "name", and PWD prints the current directory.
static void stmt_mkdir(void) {
    lex_next();
    value_t s = need_str(); if (g_err) return;
    char nm[64]; copy_fname(s, nm, sizeof nm);
    int r = stg_mkdir(nm); if (r) stg_err(r);
}
static void stmt_rmdir(void) {
    lex_next();
    value_t s = need_str(); if (g_err) return;
    char nm[64]; copy_fname(s, nm, sizeof nm);
    int r = stg_rmdir(nm); if (r) stg_err(r);
}
static void stmt_cd(void) {
    lex_next();
    value_t s = need_str(); if (g_err) return;
    char nm[64]; copy_fname(s, nm, sizeof nm);
    int r = stg_chdir(nm); if (r) stg_err(r);
}
static void stmt_pwd(void) {
    lex_next();
    con_puts(stg_cwd()); con_puts("\n");
}

// BPUT# ch, value : write a byte (numeric) or a whole string's bytes to a channel.
static void stmt_bput(void) {
    lex_next();                              // consume BPUT
    int ch = read_channel();
    if (g_err) return;
    if (!expect(T_COMMA)) return;
    value_t v = eval_expr();
    if (g_err) return;
    if (v.is_str) {                          // BPUT# ch, A$ writes the bytes verbatim
        for (int i = 0; i < v.len; i++)
            if (stg_putb(ch, (unsigned char)v.str[i]) < 0) { err("File write error"); return; }
    } else {
        if (stg_putb(ch, (int)v.num & 0xFF) < 0) { err("File write error"); return; }
    }
}

// CLOSE# ch : close one channel; CLOSE# 0 closes every open channel.
static void stmt_close(void) {
    lex_next();                              // consume CLOSE
    int ch = read_channel();
    if (g_err) return;
    if (ch == 0) stg_close_all();
    else         stg_close(ch);
}

// Scan the /seed directory once at startup and register every *keyword* seed
// (one built with SEED_KEYWORD, carrying a seed_keyword descriptor) so its
// keyword is part of the language immediately — used directly, without SEED/CALL.
// Plain SEED_EXPORT seeds are skipped here; they load on demand via SEED. Safe to
// read files mid-scan: stg_read resolves paths with its own buffers and does not
// touch the directory-enumeration cursor.
static void seed_scan_keywords(void) {
    seed_kw_n = 0;
    if (stg_diropen("/seed") != 0) return;           // no /seed dir: nothing to add
    stg_dirent ent;
    while (stg_dirnext(&ent) == 1) {
        if (ent.is_dir) continue;
        int n = 0; while (ent.name[n]) n++;           // require a ".SED" extension
        if (n < 4 || ent.name[n-4] != '.' || up(ent.name[n-3]) != 'S'
                  || up(ent.name[n-2]) != 'E' || up(ent.name[n-1]) != 'D') continue;

        char path[96];                                // "/seed/" + name
        int p = 0; const char *pre = "/seed/";
        while (*pre) path[p++] = *pre++;
        for (int i = 0; i < n && p < (int)sizeof(path) - 1; i++) path[p++] = ent.name[i];
        path[p] = 0;

        int len = stg_read(path, stg_buf, STG_BUF_SIZE);
        if (len > 0) seed_kw_register(stg_buf, len);
    }
}

// SEED h%, "FILE.SED" : load a native seed from storage into an executable slot
// and put its handle (1..SEED_MAX) into the numeric variable h%.
static void stmt_seed(void) {
    lex_next();                                  // consume SEED
    if (tok != T_VAR) { err("Expected a variable name"); return; }
    char hname[NAME_LEN];
    s_copy(hname, tok_var, NAME_LEN);
    if (name_is_str(hname)) { err("Seed handle must be a numeric variable"); return; }
    lex_next();
    if (!expect(T_COMMA)) return;
    if (tok != T_STR) { err("Expected a file name"); return; }

    char name[64];
    int n = 0;
    while (tok_str[n] && n < (int)sizeof(name) - 1) { name[n] = tok_str[n]; n++; }
    name[n] = 0;
    lex_next();

    // Native seeds live in the /seed directory on the data card. A bare file
    // name is resolved there; a name that already contains a path separator is
    // taken verbatim (an escape hatch for seeds kept elsewhere).
    char path[80];
    int p = 0, has_slash = 0;
    for (int i = 0; i < n; i++) if (name[i] == '/') has_slash = 1;
    if (!has_slash) { const char *pre = "/seed/"; while (*pre) path[p++] = *pre++; }
    for (int i = 0; i < n && p < (int)sizeof(path) - 5; i++) path[p++] = name[i];
    path[p] = 0;
    int has_dot = 0;                             // default the extension to .SED
    for (int i = 0; i < p; i++) if (path[i] == '.') has_dot = 1;
    if (!has_dot && p + 4 < (int)sizeof(path)) {
        path[p++] = '.'; path[p++] = 'S'; path[p++] = 'E'; path[p++] = 'D'; path[p] = 0;
    }

    int len = stg_read(path, stg_buf, STG_BUF_SIZE);
    if (len < 0) { stg_err(len); return; }
    if (len < (int)sizeof(struct seed_header)) { err("Not a valid seed file"); return; }

    struct seed_header hdr;                       // copy bytewise (-mstrict-align safe)
    for (int i = 0; i < (int)sizeof(hdr); i++) ((char *)&hdr)[i] = stg_buf[i];
    if (hdr.magic != SEED_MAGIC)            { err("Not a valid seed file"); return; }
    if (hdr.version > SEED_ABI_VERSION)     { err("Seed needs a newer interpreter"); return; }
    if (len > SEED_SLOT_SIZE)               { err("Seed is too big"); return; }
    if (hdr.entry_off >= (uint32_t)len)     { err("Not a valid seed file"); return; }

    int slot = -1;
    for (int i = 0; i < SEED_MAX; i++) if (!seed_loaded[i]) { slot = i; break; }
    if (slot < 0) { err("Too many seeds loaded"); return; }

    for (int i = 0; i < SEED_SLOT_SIZE; i++) seed_pool[slot][i] = 0;   // clears .bss
    for (int i = 0; i < len; i++) seed_pool[slot][i] = stg_buf[i];
    icache_sync(seed_pool[slot], (unsigned long)len);                  // make executable
    seed_entry_ptr[slot] = (seed_entry)(void *)(seed_pool[slot] + hdr.entry_off);
    seed_loaded[slot] = 1;

    var_t *v = var_find(hname);
    if (v) v->num = trunc_int(v->is_int, (double)(slot + 1));
}

// CALL h%, arg... : invoke a seed for its side effects, discarding the result.
// (CALL(...) and CALL$(...) as functions return the numeric / string result.)
static void stmt_call(void) {
    lex_next();                                  // consume CALL
    int paren = (tok == T_LP);
    if (paren) lex_next();
    (void)seed_run_collect();
    if (g_err) return;
    if (paren) expect(T_RP);
}

// --- RENUMBER ---------------------------------------------------------------
static int g_renum_start = 10, g_renum_step = 10;

// Map an old line number to its new one. References to a line that does not
// exist are left unchanged.
static int remap_line(int old) {
    for (int i = 0; i < prog_n; i++)
        if (prog[i].num == old) return g_renum_start + i * g_renum_step;
    return old;
}

// Rewrite one line's text into `out`, remapping the line-number references that
// follow GOTO / GOSUB / RESTORE / THEN / ELSE (including the comma lists of
// ON ... GOTO / ON ... GOSUB). Numeric literals in expressions are left alone.
static void renum_fixup_line(const char *in, char *out) {
    const char *p = in;
    int oi = 0;
    int linref = 0;          // 0 none, 1 GOTO/GOSUB/RESTORE list, 2 THEN/ELSE (optional)
    while (*p && oi < LINE_LEN - 12) {
        char c = *p;
        if (c == '"') {                                   // string literal: copy verbatim
            out[oi++] = c; p++;
            while (*p && *p != '"' && oi < LINE_LEN - 1) out[oi++] = *p++;
            if (*p == '"') out[oi++] = *p++;
            continue;
        }
        if (is_alpha(c)) {                                // a word: keyword or identifier
            char w[16]; int wn = 0;
            while (is_alnum(*p)) { if (wn < 15) w[wn++] = up(*p); out[oi++] = *p; p++; }
            w[wn] = 0;
            if (s_eq(w, "GOTO") || s_eq(w, "GOSUB") || s_eq(w, "RESTORE")) linref = 1;
            else if (s_eq(w, "THEN") || s_eq(w, "ELSE")) linref = 2;
            else linref = 0;
            continue;
        }
        if (is_digit(c) && linref) {                      // a line-number reference
            int num = 0;
            while (is_digit(*p)) { num = num * 10 + (*p - '0'); p++; }
            oi += uint_to_str(out + oi, remap_line(num));
            if (linref == 2) linref = 0;                  // THEN/ELSE take one line ref
            continue;
        }
        if (is_digit(c)) {                                // a plain numeric literal: copy
            while (is_digit(*p) || *p == '.') out[oi++] = *p++;
            continue;
        }
        if (c != ',' && c != ' ' && c != '\t' && linref == 1) linref = 0;  // list ended
        out[oi++] = c; p++;
    }
    out[oi] = 0;
}

// RENUMBER [start][,step] : renumber the program (default 10,10) and fix up all
// line-number references so GOTO/GOSUB/RESTORE/THEN/ELSE/ON still point correctly.
static void stmt_renumber(void) {
    lex_next();                                  // consume RENUMBER
    g_renum_start = 10; g_renum_step = 10;
    if (tok == T_NUM) { g_renum_start = (int)tok_num; lex_next(); }
    if (tok == T_COMMA) { lex_next(); if (tok == T_NUM) { g_renum_step = (int)tok_num; lex_next(); } }
    if (g_renum_start < 0) g_renum_start = 0;
    if (g_renum_step  < 1) g_renum_step  = 1;
    char tmp[LINE_LEN];
    for (int i = 0; i < prog_n; i++) {           // remap references (old numbers still in place)
        renum_fixup_line(prog[i].text, tmp);
        s_copy(prog[i].text, tmp, LINE_LEN);
    }
    for (int i = 0; i < prog_n; i++)             // then renumber the lines
        prog[i].num = g_renum_start + i * g_renum_step;
}

// --- AUTO / EDIT ------------------------------------------------------------
// State the REPL reads to pre-fill the next input line.
static int  g_auto_active = 0;        // AUTO mode: auto-number each entered line
static int  g_auto_num    = 0;        // next line number to offer
static int  g_auto_step   = 10;
static char g_prefill[LINE_LEN];      // one-shot prefill for the next line (EDIT)
static int  g_prefill_len = 0;

// AUTO [start][,step] : enter auto line-numbering. The REPL offers each line
// number for editing; pressing Return on an empty line leaves AUTO.
static void stmt_auto(void) {
    lex_next();
    int start = 10, step = 10;
    if (tok == T_NUM) { start = (int)tok_num; lex_next(); }
    if (tok == T_COMMA) { lex_next(); if (tok == T_NUM) { step = (int)tok_num; lex_next(); } }
    if (start < 0) start = 0;
    if (step  < 1) step  = 1;
    g_auto_num = start; g_auto_step = step; g_auto_active = 1;
}

// EDIT n : recall line n into the input line, ready to edit and re-enter.
static void stmt_edit(void) {
    lex_next();
    if (tok != T_NUM) { err("Expected a line number"); return; }
    int num = (int)tok_num; lex_next();
    int idx = find_line_index(num);
    if (idx < 0) { err("No such line number"); return; }
    int n = uint_to_str(g_prefill, num);
    g_prefill[n++] = ' ';
    const char *t = prog[idx].text;
    while (*t && n < LINE_LEN - 1) g_prefill[n++] = *t++;
    g_prefill[n] = 0;
    g_prefill_len = n;
}

