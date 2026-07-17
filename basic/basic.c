#include <stdint.h>
#include "console.h"
#include "storage.h"
#include "seed.h"
#include "image.h"
#include "sound.h"
#include "gpio.h"
#include "i2c.h"
#include "ttf.h"
#include "gfx.h"
#include "basic.h"

// ===========================================================================
// BerryBasiC - A small BBC-flavoured BASIC interpreter.
//
// Design notes
//   * Pure, freestanding C: no libc and no libm (math is hand-rolled), so the
//     exact same source builds for the bare-metal target and the host harness.
//   * Numbers are double-precision floating point.
//   * Program lines are stored as source text in a sorted table; the lexer
//     re-tokenises on the fly when a line runs. Crunching to byte tokens is a
//     later optimisation.
//   * Errors set a flag that unwinds the current statement/RUN without longjmp.
//
// Statements: PRINT LET INPUT DIM GOTO GOSUB RETURN FOR/NEXT IF..THEN REM END
// RUN LIST NEW. Functions: ABS INT SGN SQR SIN COS TAN ATN LOG EXP RND LEN ASC
// VAL CHR$ STR$ LEFT$ RIGHT$ MID$, constant PI. Operators ^ * / + -, relational
// = <> < > <= >=, string concatenation. Strings use a GC'd heap.
// ===========================================================================

#define LINE_LEN     128     // maximum length of a line
#define MAX_LINES   8192     // total length of a BASIC program
#define MAX_VARS     512     // total number of allowed variables in a program
#define NAME_LEN       8     // variable name incl. trailing '$' for strings
#define MAX_STR      255     // classic BASIC maximum string length
#define GCHEAP_SIZE 16384    // bytes of string storage for variables (GC'd)
#define SCRATCH_SIZE 4096    // per-statement scratch for string temporaries

#define BAS_PI     3.14159265358979323846
#define BAS_HALFPI 1.57079632679489661923
#define BAS_TWOPI  6.28318530717958647692
#define BAS_LN2    0.69314718055994530942

// ---------------------------------------------------------------------------
// The interpreter is one translation unit, split across the interp_*.inc
// fragments below purely for readability. They are #included — not compiled
// separately — so every `static` symbol and file-scope global is still shared
// across the whole interpreter, and definition/forward-declaration order is
// exactly the source order of the fragments. Keep the include order below:
// later fragments depend on declarations made in earlier ones.
// ---------------------------------------------------------------------------
#include "interp_util.inc"      // helpers, errors, FP math, number formatting
#include "interp_data.inc"      // program store, values, variables, arrays, strings
#include "interp_lexer.inc"     // keyword table + lexer
#include "interp_parse.inc"     // evaluator support helpers
#include "interp_seed.inc"      // native seeds + collections + service vtable
#include "interp_eval.inc"      // expression evaluator
#include "interp_stmt.inc"      // core statements + control flow
#include "interp_list.inc"      // LIST (plain + pretty)
#include "interp_events.inc"    // DATA/READ, events, VDU
#include "interp_files.inc"     // storage commands, modules, CAT, editor
#include "interp_hw.inc"        // sound, GPIO, I2C statements
#include "interp_graphics.inc"  // graphics + misc statements
#include "interp_control.inc"   // TRY/CATCH, dispatch, run loop
#include "interp_call.inc"      // PROC / FN call

// ---------------------------------------------------------------------------
// Top-level line handling and REPL
// ---------------------------------------------------------------------------

// Returns 1 if the line was executed immediately, 0 if it was stored.
static int process_line(char *line) {
    char *p = line;
    while (is_space(*p)) p++;
    if (is_digit(*p)) {                         // "<num> <text>" -> store/replace
        int num = 0;
        while (is_digit(*p)) { num = num * 10 + (*p - '0'); p++; }
        while (is_space(*p)) p++;
        prog_store(num, p);
        return 0;
    }
    // Immediate mode: execute the typed line directly.
    scan_defs();                                // so immediate PROC/FN calls resolve
    cur_line_idx = -1;
    g_runline = -1;
    g_branch = 0;
    g_return = 0;
    scratch_base = 0;
    exec_text(p, 0);
    // A direct "GOTO/GOSUB <line>" starts the program running from there.
    if (!g_err && g_branch) run_program(g_branch_line, g_branch_off);
    return 1;
}

void basic_init(void) {
    prog_n = 0;
    main_n = 0;
    run_depth = 0;
    n_imported = 0;
    var_n = 0;
    g_err = 0;
    g_runline = -1;
    cur_line_idx = -1;
    g_stop = 0;
    g_branch = 0;
    gosub_sp = 0;
    for_sp = 0;
    repeat_sp = 0;
    while_sp = 0;
    case_sp = 0;
    call_sp = 0;
    local_sp = 0;
    try_sp = 0;
    g_errcode = 0;
    g_errmsg[0] = 0;
    def_n = 0;
    g_return = 0;
    scratch_top = 0;
    scratch_base = 0;
    data_pc = 0;
    data_off = -1;
    time_base = 0;
    clear_vars();
    snd_init();          // bring up the audio hardware (host backend is a no-op)
    sound_reset();
    seed_scan_keywords();   // register language keywords from /seed (SEED_KEYWORD seeds)
}

void basic_repl(void) {
    static char line[LINE_LEN];
    // Boot logo (target/QEMU only). When it shows the logo it prints the banner
    // beside it and returns 1; otherwise we print the banner ourselves.
    if (!con_splash("BerryBasic (C) 2026 fritzone"))
        con_puts("BerryBasic (C) 2026 fritzone\n\n");
    for (;;) {
        const char *prompt = ">";               // BBC BASIC prompt
        int pre = 0;                             // editable prefill already in `line`
        int was_auto = g_auto_active;            // auto-numbering this line?
        if (g_auto_active) {                     // AUTO: offer the next line number
            prompt = "";
            pre = uint_to_str(line, g_auto_num);
            line[pre++] = ' ';
            line[pre] = 0;
        } else if (g_prefill_len) {              // EDIT: offer the recalled line
            s_copy(line, g_prefill, LINE_LEN);
            pre = g_prefill_len;
            g_prefill_len = 0;
        } else {
            line[0] = 0;
        }
        int n = con_getline_ed(line, LINE_LEN, pre, prompt);
        if (n < 0) return;                      // host EOF
        if (was_auto) {                          // empty entry (just the number) leaves AUTO
            const char *q = line;
            while (is_digit(*q)) q++;
            while (is_space(*q)) q++;
            if (*q == 0) { g_auto_active = 0; continue; }
        }
        g_err = 0;
        g_stop = 0;
        process_line(line);
        if (was_auto && g_auto_active && !g_err) g_auto_num += g_auto_step;
    }
}
