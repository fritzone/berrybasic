// ===========================================================================
// BerryBasiC — DATA/READ/RESTORE, event handlers (ON TIMER/PIN/MOUSE), VDU
//
// This file is a fragment of the interpreter and is #included by basic.c.
// It is NOT a standalone translation unit: it shares the single translation
// unit's static state and is compiled only as part of basic.c. Do not add it
// to the build system or compile it on its own.
// ===========================================================================
// ---------------------------------------------------------------------------
// DATA / READ / RESTORE. The data pointer walks the program's DATA statements
// using plain text scanning (no lexer state), so READ can run mid-statement.
// ---------------------------------------------------------------------------

static int  data_pc  = 0;        // prog index to scan for DATA from
static int  data_off = -1;       // offset of next item in prog[data_pc], -1 = not located
static char data_item[LINE_LEN];

static void data_reset(int line_num) {
    if (line_num < 0) data_pc = 0;
    else { int idx = find_line_index(line_num); data_pc = (idx < 0) ? prog_n : idx; }
    data_off = -1;
}

static int line_is_data(const char *t, int *body) {
    int i = 0; while (t[i] == ' ' || t[i] == '\t') i++;
    if (up(t[i]) == 'D' && up(t[i+1]) == 'A' && up(t[i+2]) == 'T' &&
        up(t[i+3]) == 'A' && !is_alnum(t[i+4])) { *body = i + 4; return 1; }
    return 0;
}

// Fetch the next DATA item into data_item[]; returns 1, or 0 when out of data.
static int data_next(int *len) {
    for (;;) {
        if (data_off < 0) {                      // locate the next DATA line
            int body;
            int m = cur_module();                // DATA is scoped to the reader's module
            while (data_pc < prog_n && prog[data_pc].module == m &&
                   !line_is_data(prog[data_pc].text, &body)) data_pc++;
            if (data_pc >= prog_n || prog[data_pc].module != m) return 0;
            data_off = body;
        }
        const char *t = prog[data_pc].text;
        int i = data_off;
        while (t[i] == ' ' || t[i] == '\t') i++;
        if (t[i] == 0) { data_pc++; data_off = -1; continue; }   // end of this DATA line

        int n = 0;
        if (t[i] == '"') {                       // quoted string item
            i++;
            while (t[i] && t[i] != '"') { if (n < LINE_LEN - 1) data_item[n++] = t[i]; i++; }
            if (t[i] == '"') i++;
        } else {                                 // unquoted: up to ',' (trim trailing space)
            int s = i;
            while (t[i] && t[i] != ',') i++;
            int e = i; while (e > s && (t[e-1] == ' ' || t[e-1] == '\t')) e--;
            for (int k = s; k < e && n < LINE_LEN - 1; k++) data_item[n++] = t[k];
        }
        while (t[i] == ' ' || t[i] == '\t') i++;
        if (t[i] == ',') i++;
        data_off = i;
        data_item[n] = 0;
        *len = n;
        return 1;
    }
}

// ---------------------------------------------------------------------------
// Event handlers: ON TIMER / ON PIN / ON MOUSE register a PROC to run when
// something happens, instead of busy-polling. Events are dispatched *between
// statements* by poll_events() (see run_program / run_body), never from a real
// interrupt, so the interpreter is never re-entered at an unsafe point. A handler
// is an ordinary parameterless PROC; while one runs, further events are held off.
// ---------------------------------------------------------------------------
#define EV_PIN_MAX 8

static struct { int active; char proc[NAME_LEN];
                long interval_us; unsigned long long last_us; } ev_timer;
static struct { int active; char proc[NAME_LEN]; int x, y, b; } ev_mouse;
static struct { int active; char proc[NAME_LEN]; } ev_key;
static struct { int active; char proc[NAME_LEN]; int pin, edge, level; } ev_pin[EV_PIN_MAX];
static int  in_event;                            // reentrancy guard while a handler runs
static unsigned long long g_frame_us;            // WAIT's ~60 Hz cadence clock

static void events_reset(void) {
    ev_timer.active = 0;
    ev_mouse.active = 0;
    ev_key.active = 0;
    for (int i = 0; i < EV_PIN_MAX; i++) {
        if (ev_pin[i].active) gpio_disarm_edge(ev_pin[i].pin);
        ev_pin[i].active = 0;
    }
    in_event = 0;
    g_frame_us = 0;
    g_pending_key = -1;
    con_backbuffer(0);                           // every program starts drawing to the screen
    con_sprite_tint(0, 0, 0, 0, 0);                 // ... and untinted
    con_target_screen();                         // ... and not redirected into a sprite
}

// Read the handler's PROC name (glued "PROCname" or spaced "PROC name") into out.
static int read_handler_proc(char *out) {
    if (tok != T_KW || tok_kw != KW_PROC) { err("Expected PROC and a handler name"); return 0; }
    if (tok_var[0]) { s_copy(out, tok_var, NAME_LEN); lex_next(); return 1; }
    lex_next();
    if (tok != T_VAR) { err("Expected PROC and a handler name"); return 0; }
    s_copy(out, tok_var, NAME_LEN); lex_next();
    return 1;
}
static int word_is(const char *w) { return tok == T_VAR && s_eq(tok_var, w); }

// ON TIMER cs PROC name   |   ON TIMER OFF
static void on_timer(void) {
    lex_next();                                  // consume TIMER
    if (word_is("OFF")) { lex_next(); ev_timer.active = 0; return; }
    long cs = (long)need_num(); if (g_err) return;
    if (cs < 1) cs = 1;
    char nm[NAME_LEN]; if (!read_handler_proc(nm)) return;
    s_copy(ev_timer.proc, nm, NAME_LEN);
    ev_timer.interval_us = cs * 10000L;
    ev_timer.last_us = con_micros();
    ev_timer.active = 1;
}

// ON MOUSE PROC name   |   ON MOUSE OFF
static void on_mouse(void) {
    lex_next();                                  // consume MOUSE
    if (word_is("OFF")) { lex_next(); ev_mouse.active = 0; return; }
    char nm[NAME_LEN]; if (!read_handler_proc(nm)) return;
    s_copy(ev_mouse.proc, nm, NAME_LEN);
    con_mouse(&ev_mouse.x, &ev_mouse.y, &ev_mouse.b);   // baseline to compare against
    ev_mouse.active = 1;
}

// ON KEY PROC name   |   ON KEY OFF
// The handler reads the triggering key with GET / GET$ / INKEY(0), which return
// the very key that fired the event (it is held in g_pending_key).
static void on_key(void) {
    lex_next();                                  // consume KEY
    if (word_is("OFF")) { lex_next(); ev_key.active = 0; return; }
    char nm[NAME_LEN]; if (!read_handler_proc(nm)) return;
    s_copy(ev_key.proc, nm, NAME_LEN);
    ev_key.active = 1;
}

// ON PIN p [RISING|FALLING] PROC name   |   ON PIN p OFF   |   ON PIN OFF
static void on_pin(void) {
    lex_next();                                  // consume PIN
    if (word_is("OFF")) { lex_next();            // cancel every pin handler
        for (int i = 0; i < EV_PIN_MAX; i++)
            if (ev_pin[i].active) { gpio_disarm_edge(ev_pin[i].pin); ev_pin[i].active = 0; }
        return;
    }
    if (!gpio_available()) { err("GPIO needs the Pi, not the host build"); return; }
    int p = (int)need_num(); if (g_err) return;
    if (p < 0 || p > 27) { err("No such pin"); return; }
    if (word_is("OFF")) { lex_next();            // cancel just this pin
        for (int i = 0; i < EV_PIN_MAX; i++)
            if (ev_pin[i].active && ev_pin[i].pin == p) { gpio_disarm_edge(p); ev_pin[i].active = 0; }
        return;
    }
    int edge = 2;                                // default: both edges
    if      (word_is("RISING"))  { edge = 1; lex_next(); }
    else if (word_is("FALLING")) { edge = 0; lex_next(); }
    char nm[NAME_LEN]; if (!read_handler_proc(nm)) return;
    int slot = -1;
    for (int i = 0; i < EV_PIN_MAX; i++) if (ev_pin[i].active && ev_pin[i].pin == p) slot = i;
    if (slot < 0) for (int i = 0; i < EV_PIN_MAX; i++) if (!ev_pin[i].active) { slot = i; break; }
    if (slot < 0) { err("Too many pin handlers"); return; }
    s_copy(ev_pin[slot].proc, nm, NAME_LEN);
    ev_pin[slot].pin = p; ev_pin[slot].edge = edge; ev_pin[slot].active = 1;
    ev_pin[slot].level = gpio_read(p);           // baseline for software change detect
    gpio_arm_edge(p, edge);
}

// WAIT : block until the next ~1/60 s frame boundary, giving an animation loop a
// steady cadence without a busy spin in the program. (There is no hardware vsync
// interrupt on this path; this is frame pacing, not tear-free page flipping.)
static void stmt_wait(void) {
    lex_next();
    const unsigned long long period = 16667;     // ~60 Hz
    unsigned long long now = con_micros();
    if (g_frame_us == 0 || now > g_frame_us + period) g_frame_us = now;   // (re)sync if idle/behind
    g_frame_us += period;
    while (con_micros() < g_frame_us) sound_pump();   // keep background audio going
}

// DELAY cs : pause for cs centiseconds (the same units as TIME and INKEY), so
// DELAY 50 waits half a second. Unlike an empty counting loop, the duration is
// real-time and independent of CPU speed. Background audio keeps playing during
// the wait; a non-positive value returns at once.
static void stmt_delay(void) {
    lex_next();                                       // consume DELAY
    double cs = need_num();
    if (g_err || cs <= 0) return;
    unsigned long long target = con_micros() + (unsigned long long)(cs * 10000.0);
    while (con_micros() < target) sound_pump();
}

static void stmt_on(void) {
    lex_next();                                  // consume ON
    if (tok == T_KW && tok_kw == KW_PIN)   { on_pin();   return; }
    if (tok == T_KW && tok_kw == KW_MOUSE) { on_mouse(); return; }
    if (word_is("TIMER"))                  { on_timer(); return; }
    if (word_is("KEY"))                    { on_key();   return; }
    int sel = (int)need_num();
    if (g_err) return;
    if (tok != T_KW || (tok_kw != KW_GOTO && tok_kw != KW_GOSUB)) { err("Expected GOTO or GOSUB after ON"); return; }
    int is_gosub = (tok_kw == KW_GOSUB);
    lex_next();
    int target = -1, i = 1;
    for (;;) {                                   // pick the sel-th line number (1-based)
        if (tok != T_NUM) { err("Expected a line number"); return; }
        if (i == sel) target = (int)tok_num;
        lex_next();
        i++;
        if (tok == T_COMMA) { lex_next(); continue; }
        break;
    }
    if (target < 0) return;                      // out of range: fall through to next line
    if (is_gosub) {
        if (cur_line_idx < 0) { err("This can only be used inside a program"); return; }
        if (gosub_sp >= GOSUB_MAX) { err("Out of memory"); return; }
        gosub_stack[gosub_sp].line = cur_line_idx;
        gosub_stack[gosub_sp].off  = cur_off();
        gosub_sp++;
    }
    branch_to_line(target);
}

// VDU n[,n...][;] : send each value's bytes to the VDU driver. A value followed
// by ';' is sent as a 16-bit word (two bytes, least-significant first); otherwise
// just its least-significant byte is sent. A trailing '|' (BBC shorthand) sends
// nine zero bytes, padding out a VDU 23 command.
static void stmt_vdu(void) {
    lex_next();                                  // consume VDU
    if (tok == T_EOL || tok == T_COLON) return;  // bare VDU does nothing
    for (;;) {
        int v = (int)need_num();
        if (g_err) return;
        if (tok == T_SEMI) {                     // 16-bit: low byte then high byte
            con_vdu(v & 0xff);
            con_vdu((v >> 8) & 0xff);
            lex_next();
        } else {                                 // 8-bit: low byte only
            con_vdu(v & 0xff);
            if (tok == T_COMMA) lex_next();
            else break;
        }
        if (tok == T_EOL || tok == T_COLON) break;
    }
}

static void stmt_restore(void) {
    lex_next();                                  // consume RESTORE
    int line = -1;
    if (tok == T_NUM) { line = (int)tok_num; lex_next(); }
    data_reset(line);
}

// READ var[,var...] : assign successive DATA items (numeric or string).
static void stmt_read(void) {
    lex_next();                                  // consume READ
    for (;;) {
        target_t t;
        if (!parse_target(&t)) return;           // variable, array element or record field
        int len;
        if (!data_next(&len)) { err("READ ran out of DATA"); return; }
        if (t.is_str) target_put_str(&t, data_item, len);
        else          target_put_num(&t, parse_double(data_item, len, 0));
        if (g_err) return;
        if (tok == T_COMMA) { lex_next(); continue; }
        break;
    }
}

