#include "interp_debug.h"
#include "interp_util.h"
#include "interp_data.h"
#include "interp_lexer.h"
#include "interp_parse.h"
#include "interp_eval.h"
#include "interp_stmt.h"
#include "interp_call.h"
#include "interp_events.h"
#include "interp_control.h"
#include "interp_files.h"
// ===========================================================================
// BerryBasiC — the debugger hook core (interp_debug.c)
//
// Mechanism only: the interpreter fires callbacks at six sites and this module
// decides whether to pause, then hands control to whatever front-end is
// attached (a C callback, a BASIC PROC, or the built-in console stepper). All
// state lives in one block that is zero-cost when disarmed (dbg_active == 0).
//
// Its cross-module interface is in interp_debug.h; the dbg_ctx passed to a
// front-end and the dbg_* service signatures are in berry_services.h.
// ===========================================================================

// --- state ------------------------------------------------------------------

int      dbg_active         = 0;
dbg_mode dbg_step_mode      = DBG_RUN;
int      dbg_step_depth     = 0;
int      dbg_break_every_line = 0;
int      dbg_step_stmt      = 0;
int      dbg_break_on_error = 0;
int      dbg_trace_fh       = 0;
int      dbg_trace_calls    = 0;
int      dbg_in_hook        = 0;
int      dbg_run_active     = 0;

dbg_ctx     dbg_cur;
dbg_bp_t    dbg_bp[DBG_MAX_BP];
int         dbg_bp_n = 0;
unsigned    dbg_bp_kw[DBG_KW_WORDS];
int         dbg_bp_kw_n = 0;
dbg_watch_t dbg_watch_set[DBG_MAX_WATCH];
int         dbg_watch_n = 0;
dbg_frame_t dbg_frames[DBG_FRAME_MAX];
int         dbg_frame_n = 0;

void   (*dbg_on_stop_cb)(const dbg_ctx *) = 0;
char     dbg_on_stop_proc[NAME_LEN] = { 0 };

// --- profiler state (per program line) --------------------------------------
// A hit count and accumulated wall time for each prog[] index, filled by the
// line hook when profiling is on. Time is attributed to a line for the interval
// from when it started running until the next line hook fires.
int                dbg_prof = 0;                 // 1 = profiling armed
unsigned           dbg_prof_count[MAX_LINES];
unsigned long long dbg_prof_us[MAX_LINES];
static int         prof_last = -1;               // prog[] index of the line in progress
static unsigned long long prof_last_us = 0;

static void dbg_prof_reset(void) {
    for (int i = 0; i < MAX_LINES; i++) { dbg_prof_count[i] = 0; dbg_prof_us[i] = 0; }
    prof_last = -1;
    prof_last_us = 0;
}

// The last statement position we stopped at, so STEP STMT does not re-stop on
// the same statement immediately after the line hook already stopped there.
static int dbg_last_line_idx = -1;
static int dbg_last_off = -1;

// --- small helpers ----------------------------------------------------------

static void dbg_put_num(double v) {
    char buf[40];
    int n = dbl_to_str(buf, v);
    con_putsn(buf, n);
}

// dbg_active is on whenever anything is armed: a breakpoint, a watch, a step
// mode, trace-to-file, break-on-error, or an attached front-end.
void dbg_refresh_active(void) {
    int on = dbg_break_every_line || dbg_step_stmt || dbg_break_on_error
          || dbg_trace_fh || dbg_bp_n || dbg_bp_kw_n || dbg_watch_n
          || dbg_on_stop_cb || dbg_on_stop_proc[0]
          || dbg_prof
          || dbg_step_mode != DBG_RUN;
    dbg_active = on ? 1 : 0;
}

// --- expression evaluation in the paused context ----------------------------

int dbg_eval_expr(const char *expr, double *num, char *str, int slen) {
    static char buf[LINE_LEN];
    lexstate_t sv;
    int save_hook = dbg_in_hook;
    int save_top  = scratch_top;
    int save_err  = g_err, save_rep = g_err_reported;
    int isstr = 0, failed = 0;
    s_copy(buf, expr, LINE_LEN);
    lex_save(&sv);
    dbg_in_hook = 1;                     // FN calls in the expression must not re-hook
    g_err = 0;                           // let this evaluation fail without disturbing
    cur_text = buf; lx = buf; lex_next();
    value_t v = eval_expr();
    if (g_err) {
        failed = 1;
    } else if (v.is_str) {
        isstr = 1;
        if (str && slen > 0) {
            int n = v.len < slen - 1 ? v.len : slen - 1;
            for (int i = 0; i < n; i++) str[i] = v.str[i];
            str[n] = 0;
        }
    } else if (num) {
        *num = v.num;
    }
    lex_restore(&sv);
    scratch_top = save_top;
    dbg_in_hook = save_hook;
    g_err = save_err; g_err_reported = save_rep;   // restore the paused program's error state
    return failed ? -1 : isstr;
}

// Returns 1 (true), 0 (false), or -1 (the condition could not be evaluated).
int dbg_cond_true(const char *cond) {
    double v = 0;
    int r = dbg_eval_expr(cond, &v, 0, 0);
    if (r < 0) return -1;                // evaluation error
    if (r == 1) return 0;               // a string result is treated as false
    return v != 0;
}

// --- keyword / watch arming predicates --------------------------------------

int dbg_kw_armed(int kw) {
    if (kw < 0 || kw >= DBG_KW_WORDS * 32) return 0;
    return (dbg_bp_kw[kw >> 5] >> (kw & 31)) & 1u;
}

int dbg_watch_armed(const char *name) {
    for (int i = 0; i < dbg_watch_n; i++)
        if (dbg_watch_set[i].enabled && !dbg_watch_set[i].show_only
            && s_eq(dbg_watch_set[i].name, name)) return 1;
    return 0;
}

// --- breakpoint bookkeeping -------------------------------------------------

static dbg_bp_t *dbg_find_line_bp(int line) {
    for (int i = 0; i < dbg_bp_n; i++)
        if (dbg_bp[i].kind == BP_LINE && dbg_bp[i].enabled && dbg_bp[i].line == line)
            return &dbg_bp[i];
    return 0;
}
static dbg_bp_t *dbg_find_proc_bp(const char *name) {
    for (int i = 0; i < dbg_bp_n; i++)
        if (dbg_bp[i].kind == BP_PROC && dbg_bp[i].enabled && s_eq(dbg_bp[i].proc, name))
            return &dbg_bp[i];
    return 0;
}

// Does a reached breakpoint actually fire? Accounts for hit count (AFTER/EVERY)
// and any condition, and disables a breakpoint whose condition itself errors.
static int dbg_bp_fires(dbg_bp_t *b) {
    b->hits++;
    if (b->hits <= b->ignore) return 0;             // AFTER n
    if (b->every > 0 && (b->hits % b->every) != 0) return 0;  // EVERY n
    if (b->cond[0]) {
        int t = dbg_cond_true(b->cond);
        if (t < 0) { b->enabled = 0; return 0; }    // a broken condition: drop the bp
        if (!t) return 0;
    }
    return 1;
}

// --- the trace-to-file path (non-stop) --------------------------------------

static void dbg_trace_puts(const char *s) { while (*s) stg_putb(dbg_trace_fh, *s++); }
static void dbg_trace_num(long v) {
    char b[16]; int n = 0;
    if (v < 0) { stg_putb(dbg_trace_fh, '-'); v = -v; }
    if (v == 0) b[n++] = '0';
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) stg_putb(dbg_trace_fh, b[--n]);
}
static void dbg_trace_line(int pc) {
    dbg_trace_puts("[");
    dbg_trace_num(prog[pc].num);
    dbg_trace_puts("] ");
    const char *t = prog[pc].text;
    while (*t) stg_putb(dbg_trace_fh, *t++);
    stg_putb(dbg_trace_fh, '\n');
}

// TRACE ...,CALLS : log a PROC/FN entry ('>') or return ('<') to the trace file.
static void dbg_trace_call(const char *name, int is_fn, int enter) {
    dbg_trace_puts(enter ? "  > " : "  < ");
    dbg_trace_puts(is_fn ? "FN" : "PROC");
    dbg_trace_puts(name);
    if (enter && g_runline >= 0) { dbg_trace_puts(" (from line "); dbg_trace_num(g_runline); dbg_trace_puts(")"); }
    stg_putb(dbg_trace_fh, '\n');
}

// --- the stop mechanism -----------------------------------------------------

// Print the stop banner + any SHOW/WATCH variables (built-in console stepper).
static void dbg_console_show_vars(void) {
    for (int i = 0; i < dbg_watch_n; i++) {
        dbg_watch_t *w = &dbg_watch_set[i];
        if (!w->enabled) continue;
        con_puts("    ");
        con_puts(w->name);
        con_puts(" = ");
        var_t *v = var_lookup(w->name);
        if (!v) { con_puts("<undefined>\n"); continue; }
        if (v->is_str) { con_putc('"'); con_putsn(v->s.sptr, v->s.slen); con_putc('"'); }
        else           { dbg_put_num(v->num); }
        con_putc('\n');
    }
}

static void dbg_console_banner(dbg_ctx *c) {
    con_puts("\n-- debug: ");
    switch (c->event) {
        case BERRY_DBG_ERROR:   con_puts("error \""); con_puts(c->errmsg ? c->errmsg : "");
                                con_puts("\""); break;
        case BERRY_DBG_CALL:    con_puts("call "); con_puts(c->name ? c->name : ""); break;
        case BERRY_DBG_WRITE:   con_puts("write "); con_puts(c->name ? c->name : ""); break;
        case BERRY_DBG_KEYWORD: { const char *s = kw_spelling(c->kw);
                                  con_puts("keyword "); con_puts(s ? s : "?"); break; }
        default:                con_puts("stopped"); break;
    }
    if (c->line >= 0) { con_puts(" at line "); con_putn(c->line); }
    con_puts("\n");
    // The source line, if we have it.
    if (c->line_idx >= 0 && c->line_idx < prog_n) {
        con_puts("    "); con_putn(prog[c->line_idx].num);
        con_puts("  "); con_puts(prog[c->line_idx].text); con_putc('\n');
    }
    dbg_console_show_vars();
    con_puts("[space]step [o]ver [u]out [c]ont [v]ars [b]t [q]uit> ");
}

// A backtrace dump used by the console stepper and the BACKTRACE statement.
static void dbg_dump_backtrace(void) {
    con_puts("Call stack (innermost first):\n");
    if (dbg_frame_n == 0) { con_puts("  (top level)\n"); return; }
    for (int i = dbg_frame_n - 1; i >= 0; i--) {
        con_puts("  ");
        con_puts(dbg_frames[i].is_fn ? "FN" : "PROC");
        con_puts(dbg_frames[i].name);
        con_puts("  called from line ");
        con_putn(dbg_frames[i].call_line);
        con_putc('\n');
    }
}

static void dbg_dump_vars(void) {
    con_puts("Variables:\n");
    for (int i = 0; i < var_n; i++) {
        if (vars[i].is_rec) continue;
        con_puts("  "); con_puts(vars[i].name); con_puts(" = ");
        if (vars[i].is_str) { con_putc('"'); con_putsn(vars[i].s.sptr, vars[i].s.slen); con_putc('"'); }
        else                { dbg_put_num(vars[i].num); }
        con_putc('\n');
    }
}

// The built-in front-end: a single-key stepper. Sets a resume decision.
static void dbg_console_stop(dbg_ctx *c) {
    for (;;) {
        dbg_console_banner(c);
        int k = con_getkey();
        con_putc('\n');
        if (k == ' ' || k == 's')      { dbg_do_step();      return; }
        else if (k == 'o')             { dbg_do_step_over(); return; }
        else if (k == 'u')             { dbg_do_step_out();  return; }
        else if (k == 'c' || k == '\r' || k == '\n') { dbg_do_cont(); return; }
        else if (k == 'q' || k == 27)  { dbg_do_abort();     return; }
        else if (k == 'v')             { dbg_dump_vars(); }
        else if (k == 'b')             { dbg_dump_backtrace(); }
        // any other key: redraw and ask again
    }
}

void dbg_stop(dbg_ctx *ctx) {
    if (dbg_in_hook) return;                 // never stop inside a stop
    dbg_in_hook = 1;
    dbg_step_mode = DBG_STOPPED;
    dbg_cur = *ctx;
    dbg_last_line_idx = ctx->line_idx;
    dbg_last_off = ctx->off;

    // A front-end must be able to run (a BASIC handler body, an expression eval)
    // even at an ERROR stop, where g_err is already set - run_body/exec bail on
    // g_err. Clear the pending error for the duration of the stop and restore it
    // afterwards, UNLESS the front-end left a new error (dbg_abort, or a fault in
    // the handler itself), which then propagates in its place.
    int se_err = g_err, se_code = g_errcode, se_line = g_errline, se_rep = g_err_reported;
    char se_msg[ERRMSG_MAX]; s_copy(se_msg, g_errmsg, ERRMSG_MAX);
    g_err = 0; g_err_reported = 0;

    if (dbg_on_stop_cb) {
        dbg_on_stop_cb(ctx);
    } else if (dbg_on_stop_proc[0]) {
        // A BASIC handler PROC. Preserve the full lexer position (the keyword
        // hook fires mid-line, so dispatch_handler's tok=T_EOL would lose it).
        lexstate_t sv; lex_save(&sv);
        int st = tok; tok = T_EOL;
        call_named(0, dbg_on_stop_proc, 0);
        (void)st;
        lex_restore(&sv);
    } else {
        dbg_console_stop(ctx);
    }

    if (dbg_step_mode == DBG_STOPPED) dbg_step_mode = DBG_RUN;   // no decision -> continue

    if (!g_err) {                            // front-end left no new error: restore
        g_err = se_err; g_errcode = se_code; g_errline = se_line;
        g_err_reported = se_rep; set_errmsg(se_msg);
    }
    dbg_in_hook = 0;
}

// --- resume decisions -------------------------------------------------------

void dbg_do_cont(void)      { dbg_step_mode = DBG_RUN; }
void dbg_do_step(void)      { dbg_step_mode = DBG_STEP; }
void dbg_do_step_over(void) { dbg_step_mode = DBG_STEP_OVER; dbg_step_depth = dbg_cur.depth; }
void dbg_do_step_out(void)  { dbg_step_mode = DBG_STEP_OUT;  dbg_step_depth = dbg_cur.depth; }
void dbg_do_abort(void)     { dbg_step_mode = DBG_RUN; err("Stopped by user"); }

// --- the six hook sites -----------------------------------------------------

void dbg_line_hook(int pc, int off) {
    if (dbg_in_hook) return;
    if (dbg_prof) {                              // per-line profiling: count + time
        unsigned long long now = con_micros();
        if (prof_last >= 0 && prof_last < MAX_LINES) dbg_prof_us[prof_last] += now - prof_last_us;
        if (pc >= 0 && pc < MAX_LINES) dbg_prof_count[pc]++;
        prof_last = pc; prof_last_us = now;
    }
    if (dbg_trace_fh) dbg_trace_line(pc);
    int ln = prog[pc].num;
    int mainline = (prog[pc].module == 0);
    // The active step mode governs. TRACE ON seeds STEP at the start of a run
    // (dbg_run_begin), so it single-steps every line, but choosing STEP OVER/OUT
    // then yields until the target call depth is reached again.
    int stop = 0;
    switch (dbg_step_mode) {
        case DBG_STEP:      stop = 1; break;
        case DBG_STEP_OVER: stop = (call_sp <= dbg_step_depth); break;
        case DBG_STEP_OUT:  stop = (call_sp <  dbg_step_depth); break;
        default: break;                                  // DBG_RUN / DBG_STOPPED
    }
    if (!stop && mainline) {
        dbg_bp_t *b = dbg_find_line_bp(ln);
        if (b && dbg_bp_fires(b)) stop = 1;
    }
    if (!stop) return;
    dbg_ctx c;
    c.event = BERRY_DBG_LINE; c.line = ln; c.line_idx = pc; c.off = off;
    c.kw = -1; c.name = 0; c.depth = call_sp; c.errmsg = 0;
    dbg_stop(&c);
}

void dbg_kw_hook(int kw) {
    if (dbg_in_hook) return;
    int stop = 0;
    // STEP STMT: stop at each statement, but not twice on one the line hook just
    // stopped at (same line + offset).
    if (dbg_step_stmt && dbg_step_mode == DBG_STEP && call_sp <= dbg_step_depth) {
        int off = (int)(tok_start - cur_text);
        if (!(cur_line_idx == dbg_last_line_idx && off == dbg_last_off)) stop = 1;
    }
    if (!stop && dbg_kw_armed(kw)) stop = 1;
    if (!stop) return;
    dbg_ctx c;
    c.event = (dbg_step_stmt && dbg_step_mode == DBG_STEP) ? BERRY_DBG_STMT : BERRY_DBG_KEYWORD;
    c.line = g_runline; c.line_idx = cur_line_idx; c.off = (int)(tok_start - cur_text);
    c.kw = kw; c.name = 0; c.depth = call_sp; c.errmsg = 0;
    dbg_stop(&c);
}

void dbg_call_hook(const char *name, int is_fn) {
    if (dbg_frame_n < DBG_FRAME_MAX) {
        s_copy(dbg_frames[dbg_frame_n].name, name, NAME_LEN);
        dbg_frames[dbg_frame_n].is_fn = is_fn;
        dbg_frames[dbg_frame_n].call_line = g_runline;
        dbg_frame_n++;
    }
    if (dbg_in_hook) return;
    if (dbg_trace_fh && dbg_trace_calls) dbg_trace_call(name, is_fn, 1);   // TRACE ,CALLS
    dbg_bp_t *b = dbg_find_proc_bp(name);
    if (!b || !dbg_bp_fires(b)) return;
    dbg_ctx c;
    c.event = BERRY_DBG_CALL; c.line = g_runline; c.line_idx = cur_line_idx;
    c.off = 0; c.kw = -1; c.name = name; c.depth = call_sp; c.errmsg = 0;
    dbg_stop(&c);
}

void dbg_ret_hook(const char *name) {
    int is_fn = (dbg_frame_n > 0) ? dbg_frames[dbg_frame_n - 1].is_fn : 0;
    if (dbg_frame_n > 0) dbg_frame_n--;
    if (!dbg_in_hook && dbg_trace_fh && dbg_trace_calls) dbg_trace_call(name, is_fn, 0);
}

void dbg_write_hook(const char *name, int is_str, double num) {
    if (dbg_in_hook) return;
    for (int i = 0; i < dbg_watch_n; i++) {
        dbg_watch_t *w = &dbg_watch_set[i];
        if (!w->enabled || w->show_only || !s_eq(w->name, name)) continue;
        if (w->cond[0] && dbg_cond_true(w->cond) != 1) continue;
        (void)is_str; (void)num;
        dbg_ctx c;
        c.event = BERRY_DBG_WRITE; c.line = g_runline; c.line_idx = cur_line_idx;
        c.off = 0; c.kw = -1; c.name = name; c.depth = call_sp; c.errmsg = 0;
        dbg_stop(&c);
        return;
    }
}

void dbg_error_hook(void) {
    if (dbg_in_hook) return;
    dbg_ctx c;
    c.event = BERRY_DBG_ERROR; c.line = g_errline; c.line_idx = cur_line_idx;
    c.off = 0; c.kw = -1; c.name = 0; c.depth = call_sp; c.errmsg = g_errmsg;
    dbg_stop(&c);
}

// --- reset ------------------------------------------------------------------

// Full clear: forget every breakpoint/watch and detach. Called from NEW (a
// different program) and available to a front-end that wants a clean slate.
void dbg_reset(void) {
    dbg_step_mode = DBG_RUN;
    dbg_step_depth = 0;
    dbg_break_every_line = 0;
    dbg_step_stmt = 0;
    dbg_break_on_error = 0;
    dbg_trace_fh = 0;
    dbg_trace_calls = 0;
    dbg_bp_n = 0;
    dbg_bp_kw_n = 0;
    for (int i = 0; i < DBG_KW_WORDS; i++) dbg_bp_kw[i] = 0;
    dbg_watch_n = 0;
    dbg_frame_n = 0;
    dbg_in_hook = 0;
    dbg_on_stop_cb = 0;
    dbg_on_stop_proc[0] = 0;
    dbg_prof = 0;
    dbg_prof_reset();
    dbg_active = 0;
}

// Per-run reset of transient state. Breakpoints PERSIST across RUN (they are
// keyed by line number and matched live), so `BREAK 20 : RUN` works; only the
// run-local bookkeeping is cleared: the call-frame stack, per-breakpoint hit
// counts, and the step mode (which starts stepping only if TRACE ON is armed).
void dbg_run_begin(void) {
    dbg_frame_n = 0;
    dbg_in_hook = 0;
    for (int i = 0; i < dbg_bp_n; i++) dbg_bp[i].hits = 0;
    if (dbg_prof) dbg_prof_reset();          // each run profiles from scratch
    dbg_step_mode = dbg_break_every_line ? DBG_STEP : DBG_RUN;
    dbg_refresh_active();
}

// ===========================================================================
// BASIC surface (TRACE / BREAK / WATCH / STEP / ... )
// ===========================================================================

// Add (or return the existing) breakpoint of `kind` at line/proc.
static dbg_bp_t *dbg_new_bp(void) {
    if (dbg_bp_n >= DBG_MAX_BP) { err("Too many breakpoints"); return 0; }
    dbg_bp_t *b = &dbg_bp[dbg_bp_n++];
    b->kind = BP_LINE; b->line = 0; b->proc[0] = 0;
    b->cond[0] = 0; b->ignore = 0; b->every = 0; b->hits = 0; b->enabled = 1;
    return b;
}

// Read an expression's source text up to end-of-statement into buf (for a
// condition), leaving the lexer at the terminating token.
static void dbg_read_rest(char *buf, int max) {
    const char *start = tok_start;
    while (tok != T_EOL && tok != T_COLON) lex_next();
    const char *end = tok_start;
    int n = (int)(end - start);
    while (n > 0 && (start[n - 1] == ' ' || start[n - 1] == '\t')) n--;
    if (n > max - 1) n = max - 1;
    for (int i = 0; i < n; i++) buf[i] = start[i];
    buf[n] = 0;
}

// TRACE ON | TRACE OFF | TRACE TO "file"[,CALLS]
void stmt_trace(void) {
    lex_next();
    if (tok == T_KW && tok_kw == KW_ON) {
        lex_next();
        dbg_break_every_line = 1;
        dbg_step_mode = DBG_STEP;              // stop before the very next line
        dbg_refresh_active();
        return;
    }
    if (word_is("OFF")) {
        lex_next();
        dbg_break_every_line = 0;
        if (dbg_trace_fh) { stg_close(dbg_trace_fh); dbg_trace_fh = 0; }
        dbg_refresh_active();
        return;
    }
    if (tok == T_KW && tok_kw == KW_TO) {      // TO is the FOR..TO keyword, not a word
        lex_next();
        value_t f = need_str();
        if (g_err) return;
        char path[64];
        int n = f.len < (int)sizeof(path) - 1 ? f.len : (int)sizeof(path) - 1;
        for (int i = 0; i < n; i++) path[i] = f.str[i];
        path[n] = 0;
        int fh = stg_open(path, STG_M_WRITE);
        if (!fh) { err("TRACE TO: cannot open the file"); return; }
        dbg_trace_fh = fh;
        dbg_trace_calls = 0;
        if (tok == T_COMMA) { lex_next(); if (word_is("CALLS")) { lex_next(); dbg_trace_calls = 1; } }
        dbg_refresh_active();
        return;
    }
    err("Expected ON, OFF or TO");
}

// BREAK line [IF expr] [EVERY n | AFTER n]
// BREAK PROC name | BREAK KEYWORD "PRINT" | BREAK ERROR
void stmt_break(void) {
    lex_next();
    if (tok == T_KW && tok_kw == KW_PROC) {                 // BREAK PROC name
        char nm[NAME_LEN];
        if (tok_var[0]) { s_copy(nm, tok_var, NAME_LEN); lex_next(); }
        else { lex_next(); if (!read_name_word(nm, "Expected a PROC name")) return; }
        dbg_bp_t *b = dbg_new_bp(); if (!b) return;
        b->kind = BP_PROC; s_copy(b->proc, nm, NAME_LEN);
        dbg_refresh_active();
        return;
    }
    if (word_is("KEYWORD")) {                               // BREAK KEYWORD "PRINT"
        lex_next();
        value_t s = need_str(); if (g_err) return;
        char kw[16];
        int n = s.len < 15 ? s.len : 15;
        for (int i = 0; i < n; i++) kw[i] = up(s.str[i]);
        kw[n] = 0;
        int id = -1;
        extern const kwent_t kwtab[]; extern const int kwcount;
        for (int i = 0; i < kwcount; i++) if (s_eq(kw, kwtab[i].name)) { id = kwtab[i].id; break; }
        if (id < 0 || id >= DBG_KW_WORDS * 32) { err("Unknown keyword"); return; }
        if (!dbg_kw_armed(id)) { dbg_bp_kw[id >> 5] |= (1u << (id & 31)); dbg_bp_kw_n++; }
        dbg_refresh_active();
        return;
    }
    if (word_is("ERROR")) { lex_next(); dbg_break_on_error = 1; dbg_refresh_active(); return; }
    // BREAK line ...
    int line = (int)need_num(); if (g_err) return;
    dbg_bp_t *b = dbg_new_bp(); if (!b) return;
    b->kind = BP_LINE; b->line = line;
    if (tok == T_KW && tok_kw == KW_IF) {                   // IF expr
        lex_next();
        dbg_read_rest(b->cond, DBG_COND_LEN);
    }
    if (word_is("EVERY")) { lex_next(); b->every = (int)need_num(); }
    else if (word_is("AFTER")) { lex_next(); b->ignore = (int)need_num(); }
    dbg_refresh_active();
}

// UNBREAK line | UNBREAK ALL | UNBREAK ERROR
void stmt_unbreak(void) {
    lex_next();
    if (word_is("ALL")) {
        lex_next();
        dbg_bp_n = 0; dbg_bp_kw_n = 0;
        for (int i = 0; i < DBG_KW_WORDS; i++) dbg_bp_kw[i] = 0;
        dbg_break_on_error = 0;
        dbg_refresh_active();
        return;
    }
    if (word_is("ERROR")) { lex_next(); dbg_break_on_error = 0; dbg_refresh_active(); return; }
    int line = (int)need_num(); if (g_err) return;
    for (int i = 0; i < dbg_bp_n; i++)
        if (dbg_bp[i].kind == BP_LINE && dbg_bp[i].line == line) {
            for (int j = i; j < dbg_bp_n - 1; j++) dbg_bp[j] = dbg_bp[j + 1];
            dbg_bp_n--; i--;
        }
    dbg_refresh_active();
}

static void dbg_add_watch(int show_only) {
    if (dbg_watch_n >= DBG_MAX_WATCH) { err("Too many watches"); return; }
    if (tok != T_VAR) { err("Expected a variable name"); return; }
    dbg_watch_t *w = &dbg_watch_set[dbg_watch_n++];
    s_copy(w->name, tok_var, NAME_LEN);
    w->cond[0] = 0; w->show_only = show_only; w->enabled = 1;
    lex_next();
    if (!show_only && tok == T_KW && tok_kw == KW_IF) { lex_next(); dbg_read_rest(w->cond, DBG_COND_LEN); }
    dbg_refresh_active();
}

void stmt_watch(void) { lex_next(); dbg_add_watch(0); }   // WATCH var [IF expr]
void stmt_show(void)  { lex_next(); dbg_add_watch(1); }   // SHOW var

// STEP [OVER|OUT|IN|STMT] — a resume decision (used at a stop, e.g. ON DEBUG).
void stmt_step(void) {
    lex_next();
    if (word_is("OVER"))      { lex_next(); dbg_do_step_over(); }
    else if (word_is("OUT"))  { lex_next(); dbg_do_step_out(); }
    else if (word_is("STMT")) { lex_next(); dbg_step_stmt = 1; dbg_do_step(); dbg_refresh_active(); }
    else                      { if (word_is("IN")) lex_next(); dbg_do_step(); }
}

void stmt_cont(void)      { lex_next(); dbg_do_cont(); }
void stmt_backtrace(void) { lex_next(); dbg_dump_backtrace(); }

// Print the profile: each executed line with its hit count and total time,
// hottest first. Uses a floor-tracking selection so it needs no scratch array.
static void dbg_profile_report(void) {
    int any = 0;
    for (int i = 0; i < prog_n; i++) if (dbg_prof_count[i]) { any = 1; break; }
    if (!any) { con_puts("\nNo profile data. Use PROFILE ON, then RUN, then PROFILE.\n"); return; }
    con_puts("\n  line     hits        ms  source\n");
    unsigned long long fus = ~0ULL; int fidx = 0x7fffffff;
    for (int shown = 0; shown < 30; shown++) {
        unsigned long long bus = 0; int bidx = -1;
        for (int i = 0; i < prog_n; i++) {
            if (!dbg_prof_count[i]) continue;
            unsigned long long u = dbg_prof_us[i];
            if (u > fus || (u == fus && i <= fidx)) continue;        // not below the floor
            //   order is (time desc, line asc); "below the floor" = smaller in that order
            if (bidx < 0 || u > bus || (u == bus && i < bidx)) { bus = u; bidx = i; }
        }
        if (bidx < 0) break;
        fus = bus; fidx = bidx;
        // "  NNNN  COUNT  MS  text"  (line number right-aligned in 5 columns)
        con_puts("  ");
        { char t[12]; int n = 0, v = prog[bidx].num;
          do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
          for (int p = n; p < 5; p++) con_putc(' ');
          while (n) con_putc(t[--n]); }
        con_puts("  ");
        con_putn((long)dbg_prof_count[bidx]);
        con_puts("   ");
        { char b[40]; int n = dbl_to_str(b, (double)dbg_prof_us[bidx] / 1000.0); con_putsn(b, n); }
        con_puts("  ");
        { const char *s = prog[bidx].text; for (int k = 0; k < 40 && s[k]; k++) con_putc(s[k]); }
        con_putc('\n');
    }
}

// PROFILE ON | PROFILE OFF | PROFILE  (bare = print the report).
void stmt_profile(void) {
    lex_next();
    if (tok == T_KW && tok_kw == KW_ON)  { lex_next(); dbg_prof = 1; dbg_prof_reset(); dbg_refresh_active(); return; }
    if (word_is("OFF"))                  { lex_next(); dbg_prof = 0; dbg_refresh_active(); return; }
    dbg_profile_report();
}

// SET var = expr — change a variable (used while stopped).
void stmt_dbgset(void) {
    lex_next();
    if (tok != T_VAR) { err("Expected a variable name"); return; }
    char name[NAME_LEN]; s_copy(name, tok_var, NAME_LEN);
    lex_next();
    if (tok != T_EQ) { err("Expected '='"); return; }
    lex_next();
    value_t v = eval_expr();
    if (g_err) return;
    var_t *var = var_find(name);
    if (!var) return;
    if (var->is_str) {
        if (!v.is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
        str_store(var, v.str, v.len);
    } else {
        if (v.is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
        var->num = trunc_int(var->is_int, v.num);
    }
}

int dbg_line_value(void) {
    return (dbg_step_mode == DBG_STOPPED) ? dbg_cur.line : 0;
}

// ===========================================================================
// Services (dbg_* in BerryServices) — for POD / seed front-ends
// ===========================================================================

void dbg_svc_attach(void (*on_stop)(const dbg_ctx *)) {
    dbg_on_stop_cb = on_stop;
    dbg_refresh_active();
}
void dbg_svc_detach(void) {
    dbg_on_stop_cb = 0;
    dbg_refresh_active();
}

int dbg_svc_break_line(int line) {
    dbg_bp_t *b = dbg_new_bp(); if (!b) return -1;
    b->kind = BP_LINE; b->line = line;
    dbg_refresh_active();
    return 0;
}
int dbg_svc_break_line_if(int line, const char *cond, int ignore) {
    dbg_bp_t *b = dbg_new_bp(); if (!b) return -1;
    b->kind = BP_LINE; b->line = line; b->ignore = ignore;
    if (cond) s_copy(b->cond, cond, DBG_COND_LEN);
    dbg_refresh_active();
    return 0;
}
int dbg_svc_break_proc(const char *name) {
    dbg_bp_t *b = dbg_new_bp(); if (!b) return -1;
    b->kind = BP_PROC; s_copy(b->proc, name, NAME_LEN);
    dbg_refresh_active();
    return 0;
}
int dbg_svc_break_kw(const char *keyword) {
    extern const kwent_t kwtab[]; extern const int kwcount;
    char kw[16]; int i = 0;
    for (; keyword[i] && i < 15; i++) kw[i] = up(keyword[i]);
    kw[i] = 0;
    int id = -1;
    for (int j = 0; j < kwcount; j++) if (s_eq(kw, kwtab[j].name)) { id = kwtab[j].id; break; }
    if (id < 0 || id >= DBG_KW_WORDS * 32) return -1;
    if (!dbg_kw_armed(id)) { dbg_bp_kw[id >> 5] |= (1u << (id & 31)); dbg_bp_kw_n++; }
    dbg_refresh_active();
    return 0;
}
void dbg_svc_break_every(int on) {
    dbg_break_every_line = on ? 1 : 0;
    if (on) dbg_step_mode = DBG_STEP;
    dbg_refresh_active();
}
void dbg_svc_clear(int line) {
    if (line == 0) {
        dbg_bp_n = 0; dbg_bp_kw_n = 0;
        for (int i = 0; i < DBG_KW_WORDS; i++) dbg_bp_kw[i] = 0;
    } else {
        for (int i = 0; i < dbg_bp_n; i++)
            if (dbg_bp[i].kind == BP_LINE && dbg_bp[i].line == line) {
                for (int j = i; j < dbg_bp_n - 1; j++) dbg_bp[j] = dbg_bp[j + 1];
                dbg_bp_n--; i--;
            }
    }
    dbg_refresh_active();
}
int dbg_svc_list_breaks(int *lines, int max) {
    int n = 0;
    for (int i = 0; i < dbg_bp_n && n < max; i++)
        if (dbg_bp[i].kind == BP_LINE) lines[n++] = dbg_bp[i].line;
    return n;
}
int dbg_svc_watch(const char *var, const char *cond) {
    if (dbg_watch_n >= DBG_MAX_WATCH) return -1;
    dbg_watch_t *w = &dbg_watch_set[dbg_watch_n++];
    s_copy(w->name, var, NAME_LEN);
    w->cond[0] = 0; w->show_only = 0; w->enabled = 1;
    if (cond) s_copy(w->cond, cond, DBG_COND_LEN);
    dbg_refresh_active();
    return 0;
}
void dbg_svc_break_error(int on) { dbg_break_on_error = on ? 1 : 0; dbg_refresh_active(); }
void dbg_svc_trace_to(int fh, int include_calls) {
    dbg_trace_fh = fh; dbg_trace_calls = include_calls ? 1 : 0;
    dbg_refresh_active();
}
void dbg_svc_trace_off(void) { dbg_trace_fh = 0; dbg_refresh_active(); }

int dbg_svc_where(dbg_ctx *out) {
    if (!out) return -1;
    *out = dbg_cur;
    return (dbg_step_mode == DBG_STOPPED) ? 0 : 1;   // 1 = not currently stopped
}
int dbg_svc_set_num(const char *name, double v) {
    var_t *var = var_find(name);
    if (!var || var->is_str || var->is_rec) return -1;
    var->num = trunc_int(var->is_int, v);
    return 0;
}
int dbg_svc_set_str(const char *name, const char *s, int n) {
    var_t *var = var_find(name);
    if (!var || !var->is_str || var->is_rec) return -1;
    str_store(var, s, n);
    return 0;
}
int dbg_svc_var_count(void) { return var_n + arr_n; }
int dbg_svc_var_at(int i, char *name, int namelen, int *is_str, int *is_arr) {
    if (i < 0) return -1;
    if (i < var_n) {
        s_copy(name, vars[i].name, namelen);
        if (is_str) *is_str = vars[i].is_str;
        if (is_arr) *is_arr = 0;
        return 0;
    }
    i -= var_n;
    if (i < arr_n) {
        s_copy(name, arrs[i].name, namelen);
        if (is_str) *is_str = arrs[i].is_str;
        if (is_arr) *is_arr = 1;
        return 0;
    }
    return -1;
}
int dbg_svc_stack_depth(void) { return dbg_frame_n; }
int dbg_svc_stack_frame(int i, char *name, int namelen, int *call_line) {
    // frame 0 = innermost
    if (i < 0 || i >= dbg_frame_n) return -1;
    int idx = dbg_frame_n - 1 - i;
    s_copy(name, dbg_frames[idx].name, namelen);
    if (call_line) *call_line = dbg_frames[idx].call_line;
    return 0;
}
int dbg_svc_line_count(void) { return main_n; }
int dbg_svc_line_at(int idx, int *number, char *text, int textlen) {
    if (idx < 0 || idx >= prog_n) return -1;
    if (number) *number = prog[idx].num;
    if (text) s_copy(text, prog[idx].text, textlen);
    return 0;
}

// dbg_run: run a BASIC program with the debugger armed. The breakpoints and
// attachment the front-end set beforehand are preserved across the run's reset
// (dbg_run_active gates dbg_reset), then cleared when it returns.
int svc_run_basic(const char *path);   // interp_seed.c
int dbg_svc_run(const char *path) {
    dbg_run_active = 1;
    dbg_refresh_active();
    int r = svc_run_basic(path);
    dbg_run_active = 0;
    return r;
}
