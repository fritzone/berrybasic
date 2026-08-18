#include "interp_util.h"
#include "interp_data.h"
#include "interp_lexer.h"
#include "interp_parse.h"
#include "interp_seed.h"
#include "interp_eval.h"
#include "interp_stmt.h"
#include "interp_list.h"
#include "interp_events.h"
#include "interp_files.h"
#include "interp_hw.h"
#include "interp_graphics.h"
#include "interp_pod.h"
#include "interp_control.h"
#include "interp_call.h"


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



// ---------------------------------------------------------------------------
// The interpreter is split across interp_*.c modules, each compiled separately.
// The shared interface lives in headers:
//   * interp_base.h   - platform/library includes + shared compile-time constants
//   * interp_types.h  - every data type (sectioned by owning module)
//   * interp_<mod>.h  - that module's extern globals + documented function protos
// Each interp_<mod>.c includes its own header plus the headers of the modules it
// calls into. This file (basic.c) holds only the top-level line handling + REPL,
// and pulls in every module header since it drives them all.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Top-level line handling and REPL
// ---------------------------------------------------------------------------

// Returns 1 if the line was executed immediately, 0 if it was stored.
int process_line(char *line) {
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
    else if (!g_err && chain_qn > 0) run_chain_queue();   // immediate-mode CHAIN
    return 1;
}

void basic_init(void) {
    prog_n = 0;
    main_n = 0;
    run_depth = 0;
    chain_reset();
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
    try_open = 0;
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
    if (!con_splash(COPYRIGHT_MESSAGE))
    {
        con_puts(COPYRIGHT_MESSAGE"\n\n");
    }
    for (;;) {
        const char *prompt = ">";                // BBC BASIC prompt
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
