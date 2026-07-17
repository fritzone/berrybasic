// ===========================================================================
// BerryBasiC — graphics & misc statements: SCREEN, GCOL, shapes, sprites, fonts, collections
//
// This file is a fragment of the interpreter and is #included by basic.c.
// It is NOT a standalone translation unit: it shares the single translation
// unit's static state and is compiled only as part of basic.c. Do not add it
// to the build system or compile it on its own.
// ===========================================================================
static void stmt_screen(void) {
    lex_next();                                  // consume SCREEN
    if (tok == T_EOL || tok == T_COLON) {        // bare SCREEN -> restore startup
        con_screen(0, 0);
        return;
    }
    int w = (int)need_num();   if (!expect(T_COMMA)) return;
    int h = (int)need_num();   if (g_err) return;
    if (!con_screen(w, h)) err("Could not set that screen resolution");
}

// KEYBOARD "NO" : select the keyboard layout by two-letter code (US/UK/NO/DK/SE/
// DE, case-insensitive). Affects how physical key presses are decoded from here
// on; the current layout can be read back with the KEYBOARD$ function.
static void stmt_keyboard(void) {
    lex_next();                                  // consume KEYBOARD
    value_t s = need_str();   if (g_err) return;
    char code[8];
    int n = s.len; if (n > (int)sizeof code - 1) n = (int)sizeof code - 1;
    for (int i = 0; i < n; i++) code[i] = s.str[i];
    code[n] = 0;
    if (!con_set_keyboard(code)) err("Unknown keyboard layout");
}

// POKE addr, byte : store one byte at a memory address. Familiar alias for the
// indirection form `?addr = byte`; use `!addr = word` / `$addr = "..."` for a
// 32-bit word or a string. On this bare-metal machine the address is real, so
// poke inside a DIMed buffer (see DIM name size) unless you mean a hardware one.
static void stmt_poke_kw(void) {
    lex_next();                                  // consume POKE
    long a = (long)need_num();   if (!expect(T_COMMA)) return;
    long v = (long)need_num();   if (g_err) return;
    mem_pokeb(a, v);
}

// EXEC "statement" : run a string as if it were a line of BASIC, in the current
// context (same variables, arrays, open loops). The companion to EVAL: EVAL
// computes a value, EXEC performs actions. Build the string however you like —
// a config file line, an assembled "LET " + n$ + " = " + v$, a typed command.
// The lexer is pointed at a private copy of the string and restored afterwards,
// so the statement that issued EXEC carries on normally (e.g. EXEC c$ : PRINT).
// A branch (GOTO/GOSUB) or END inside the string propagates out as usual; a
// half-open block (a FOR with no NEXT, say) is a mistake, because the copied
// text is gone once EXEC returns.
static void stmt_exec(void) {
    lex_next();                                  // consume EXEC
    value_t s = need_str();   if (g_err) return;
    char buf[LINE_LEN];
    int n = s.len; if (n > LINE_LEN - 1) n = LINE_LEN - 1;
    for (int i = 0; i < n; i++) buf[i] = s.str[i];
    buf[n] = 0;
    lexstate_t save; lex_save(&save);
    exec_text(buf, 0);
    lex_restore(&save);
}

// --- Graphics statements ----------------------------------------------------
static void stmt_mode(void) {
    lex_next();                                  // consume MODE
    int n = (int)need_num();
    if (g_err) return;
    con_mode(n);
}

// A packed colour from RGB() carries bit 30, so it can't be mistaken for a
// logical colour index (0..15 / 128..143).
#define RGB_TAG 0x40000000
static void gcol_apply_packed(int packed) {      // truecolour foreground
    con_gcol_rgb((packed >> 16) & 255, (packed >> 8) & 255, packed & 255);
}

// GCOL colour            (action 0)
// GCOL action, colour    (colour = logical index, or a packed RGB() value)
// GCOL r, g, b           (24-bit truecolour foreground)
static void stmt_gcol(void) {
    lex_next();                                  // consume GCOL
    int a = (int)need_num(); if (g_err) return;
    if (tok != T_COMMA) {                        // GCOL c
        if (a & RGB_TAG) gcol_apply_packed(a);
        else             con_gcol(0, a);
        return;
    }
    lex_next();
    int b = (int)need_num(); if (g_err) return;
    if (tok != T_COMMA) {                         // GCOL action, colour
        if (b & RGB_TAG) { con_gcol(a, 0); gcol_apply_packed(b); }
        else             con_gcol(a, b);
        return;
    }
    lex_next();
    int c = (int)need_num(); if (g_err) return;   // GCOL r, g, b
    con_gcol_rgb(a, b, c);
}

// Read `n` more comma-separated numbers into out[]. Returns 0 on error.
static int read_nums(int *out, int n) {
    for (int i = 0; i < n; i++) {
        if (i && !expect(T_COMMA)) return 0;
        out[i] = (int)need_num();
        if (g_err) return 0;
    }
    return 1;
}

// LINE x1,y1,x2,y2
static void stmt_line(void) {
    lex_next(); int v[4];
    if (!read_nums(v, 4)) return;
    con_line(v[0], v[1], v[2], v[3]);
}

// After a shape keyword, an optional FILL modifier means the solid variant.
static int shape_fill(void) {
    if (tok == T_KW && tok_kw == KW_FILL) { lex_next(); return 1; }
    return 0;
}

// RECTANGLE [FILL] x,y,w,h
static void stmt_rectangle(void) {
    lex_next(); int f = shape_fill(); int v[4];
    if (!read_nums(v, 4)) return;
    con_rectangle(v[0], v[1], v[2], v[3], f);
}

// CIRCLE [FILL] x,y,r
static void stmt_circle(void) {
    lex_next(); int f = shape_fill(); int v[3];
    if (!read_nums(v, 3)) return;
    con_circle(v[0], v[1], v[2], f);
}

// ELLIPSE [FILL] x,y,rx,ry
static void stmt_ellipse(void) {
    lex_next(); int f = shape_fill(); int v[4];
    if (!read_nums(v, 4)) return;
    con_ellipse(v[0], v[1], v[2], v[3], f);
}

// FILL x,y : flood fill from a point
static void stmt_fill(void) {
    lex_next(); int v[2];
    if (!read_nums(v, 2)) return;
    con_fill(v[0], v[1]);
}

// GGET addr, x1,y1,x2,y2 : capture a screen rectangle into a DIM buffer
static void stmt_gget(void) {
    lex_next();
    long addr = (long)need_num(); if (!expect(T_COMMA)) return;
    int v[4];
    if (!read_nums(v, 4)) return;
    con_sprite_get(addr, v[0], v[1], v[2], v[3]);
}

// GPUT addr,x,y [,scale,angle] : stamp a sprite, optionally scaled and rotated.
static void stmt_gput(void) {
    lex_next();
    long addr = (long)need_num(); if (!expect(T_COMMA)) return;
    int x = (int)need_num(); if (!expect(T_COMMA)) return;
    int y = (int)need_num();
    if (tok == T_COMMA) {                         // extended form: , scale , angle
        lex_next();
        double sc = need_num(); if (!expect(T_COMMA)) return;
        double an = need_num(); if (g_err) return;
        con_sprite_put_ex(addr, x, y, sc, an);
        return;
    }
    if (g_err) return;
    con_sprite_put(addr, x, y);                    // plain fast path
}

// GTINT r,g,b,a | GTINT OFF : tint every subsequently blitted sprite.
static void stmt_gtint(void) {
    lex_next();                                   // consume GTINT
    if (word_is("OFF")) { lex_next(); con_sprite_tint(0, 0, 0, 0, 0); return; }
    int v[4];
    if (!read_nums(v, 4)) return;                  // r,g,b,a
    con_sprite_tint(1, v[0], v[1], v[2], v[3]);
}

// NEWSPRITE addr, w, h : initialise a DIM buffer as a blank transparent sprite.
static void stmt_newsprite(void) {
    lex_next();
    long addr = (long)need_num(); if (!expect(T_COMMA)) return;
    int wh[2];
    if (!read_nums(wh, 2)) return;                 // w, h
    if (wh[0] <= 0 || wh[1] <= 0) { err("Bad sprite size"); return; }
    con_newsprite(addr, wh[0], wh[1]);
}

// SPRITETARGET addr | SPRITETARGET OFF : redirect drawing into a sprite (or back).
static void stmt_spritetarget(void) {
    lex_next();
    if (word_is("OFF")) { lex_next(); con_target_screen(); return; }
    long addr = (long)need_num(); if (g_err) return;
    if (con_target_sprite(addr) < 0) err("Bad sprite");
}

// TILEMAP sheet,map,cols,rows,tilew,tileh,scrollx,scrolly : draw a tile grid.
static void stmt_tilemap(void) {
    lex_next();
    long sheet = (long)need_num(); if (!expect(T_COMMA)) return;
    long map   = (long)need_num(); if (!expect(T_COMMA)) return;
    int v[6];                                      // cols,rows,tilew,tileh,scrollx,scrolly
    if (!read_nums(v, 6)) return;
    con_tilemap(sheet, map, v[0], v[1], v[2], v[3], v[4], v[5]);
}

// --- TrueType font statements -----------------------------------------------
// FONT handle : make a font loaded with LOADFONT the current one.
static void stmt_font(void) {
    lex_next();
    int h = (int)need_num(); if (g_err) return;
    if (!ttf_select(h)) err("No such font");
}

// FONTSIZE pixels : set the current font's glyph height.
static void stmt_fontsize(void) {
    lex_next();
    int px = (int)need_num(); if (g_err) return;
    if (px < 1) { err("Invalid argument"); return; }
    ttf_set_size(px);
}

// FONTSTYLE bold [, italic [, underline]] : each a 0/1 flag (default 0).
static void stmt_fontstyle(void) {
    lex_next();
    int bold = (int)need_num(); if (g_err) return;
    int italic = 0, underline = 0;
    if (tok == T_COMMA) { lex_next(); italic = (int)need_num(); if (g_err) return; }
    if (tok == T_COMMA) { lex_next(); underline = (int)need_num(); if (g_err) return; }
    ttf_set_style(bold, italic, underline);
}

// GTEXT x, y, s$ : draw text with the current font at logical baseline (x,y).
static void stmt_gtext(void) {
    lex_next();
    int x = (int)need_num(); if (!expect(T_COMMA)) return;
    int y = (int)need_num(); if (!expect(T_COMMA)) return;
    value_t s = need_str(); if (g_err) return;
    con_gtext(x, y, s.str, s.len);
}

// --- Collection statements --------------------------------------------------
// Copy a just-read string key into a stable local buffer: the value expression
// that follows may run the GC and move the key's bytes in the string heap.
static int grab_key(value_t k, char *buf) {
    int n = k.len; if (n > MAX_STR) n = MAX_STR;
    for (int i = 0; i < n; i++) buf[i] = k.str[i];
    return n;
}

// DICTSET d, key$, value   |   DICTDEL d, key$
static void stmt_dictset(void) {
    lex_next();
    double h = need_num(); if (!expect(T_COMMA)) return;
    value_t k = need_str(); if (g_err) return;
    char key[MAX_STR]; int klen = grab_key(k, key);
    if (!expect(T_COMMA)) return;
    value_t v = eval_expr(); if (g_err) return;
    dict_t *D = (dict_t *)coll_get(h, CT_DICT); if (!D) return;
    dict_set(D, key, klen, v);
}
static void stmt_dictdel(void) {
    lex_next();
    double h = need_num(); if (!expect(T_COMMA)) return;
    value_t k = need_str(); if (g_err) return;
    char key[MAX_STR]; int klen = grab_key(k, key);
    dict_t *D = (dict_t *)coll_get(h, CT_DICT); if (!D) return;
    dict_del(D, key, klen);
}

// PUSH L, value  |  LISTSET L, i, value  |  LISTINS L, i, value  |  LISTDEL L, i
static void stmt_push(void) {
    lex_next();
    double h = need_num(); if (!expect(T_COMMA)) return;
    value_t v = eval_expr(); if (g_err) return;
    list_t *L = (list_t *)coll_get(h, CT_LIST); if (!L) return;
    list_ins(L, L->len, v);                        // append
}
static void stmt_listset(void) {
    lex_next();
    double h = need_num(); if (!expect(T_COMMA)) return;
    int i = (int)need_num(); if (!expect(T_COMMA)) return;
    value_t v = eval_expr(); if (g_err) return;
    list_t *L = (list_t *)coll_get(h, CT_LIST); if (!L) return;
    if (i < 0 || i >= L->len) { err("Index out of range"); return; }
    cval_store(&L->item[i], v);
}
static void stmt_listins(void) {
    lex_next();
    double h = need_num(); if (!expect(T_COMMA)) return;
    int i = (int)need_num(); if (!expect(T_COMMA)) return;
    value_t v = eval_expr(); if (g_err) return;
    list_t *L = (list_t *)coll_get(h, CT_LIST); if (!L) return;
    list_ins(L, i, v);
}
static void stmt_listdel(void) {
    lex_next();
    double h = need_num(); if (!expect(T_COMMA)) return;
    int i = (int)need_num(); if (g_err) return;
    list_t *L = (list_t *)coll_get(h, CT_LIST); if (!L) return;
    list_del(L, i);
}

// TREESET t, key, value   |   TREEDEL t, key
static void stmt_treeset(void) {
    lex_next();
    double h = need_num(); if (!expect(T_COMMA)) return;
    double key = need_num(); if (!expect(T_COMMA)) return;
    value_t v = eval_expr(); if (g_err) return;
    tree_t *T = (tree_t *)coll_get(h, CT_TREE); if (!T) return;
    tree_set(T, key, v);
}
static void stmt_treedel(void) {
    lex_next();
    double h = need_num(); if (!expect(T_COMMA)) return;
    double key = need_num(); if (g_err) return;
    tree_t *T = (tree_t *)coll_get(h, CT_TREE); if (!T) return;
    tree_del(T, key);
}

// BUFFER ON | BUFFER OFF : enable/disable off-screen drawing (double buffering).
// While on, every draw targets the back buffer and the screen freezes until FLIP.
static void stmt_buffer(void) {
    lex_next();                                   // consume BUFFER
    if (tok == T_KW && tok_kw == KW_ON) {
        lex_next();
        if (con_backbuffer(1) < 0) err("Could not allocate the back buffer");
    } else if (word_is("OFF")) {
        lex_next();
        con_backbuffer(0);
    } else {
        err("Expected ON or OFF");
    }
}

// FLIP : present the back buffer to the screen (a no-op when buffering is off).
static void stmt_flip(void) {
    lex_next();                                   // consume FLIP
    con_flip();
}

// SAVESPRITE addr, "file" : write a sprite (LOADSPRITE result or GGET capture)
// out as an image file (PNG, or BMP if the name ends in .bmp).
static void stmt_savesprite(void) {
    lex_next();
    long addr = (long)need_num(); if (!expect(T_COMMA)) return;
    value_t s = need_str(); if (g_err) return;
    char nm[64]; copy_fname(s, nm, sizeof nm);
    if (img_save_sprite(nm, addr) != 0) err("Could not save sprite");
}

// PLOT code,x,y
static void stmt_plot(void) {
    lex_next();                                  // consume PLOT
    int k = (int)need_num(); if (!expect(T_COMMA)) return;
    int x = (int)need_num(); if (!expect(T_COMMA)) return;
    int y = (int)need_num(); if (g_err) return;
    con_plot(k, x, y);
}

// MOVE x,y  ==  PLOT 4,x,y     DRAW x,y  ==  PLOT 5,x,y
static void stmt_move_draw(int plotcode) {
    lex_next();                                  // consume MOVE/DRAW
    int x = (int)need_num(); if (!expect(T_COMMA)) return;
    int y = (int)need_num(); if (g_err) return;
    con_plot(plotcode, x, y);
}

