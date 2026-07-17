// ===========================================================================
// BerryBasiC — TRY/CATCH, statement dispatch, event polling, the run loop
//
// This file is a fragment of the interpreter and is #included by basic.c.
// It is NOT a standalone translation unit: it shares the single translation
// unit's static state and is compiled only as part of basic.c. Do not add it
// to the build system or compile it on its own.
// ===========================================================================
// --- TRY / CATCH / ENDTRY : structured error handling -----------------------
// Forward-scan from just after TRY for the matching CATCH (honouring nested
// TRY blocks). Records its position in *pc/*off (the start of the handler body).
// Returns 1 on success; raises and returns 0 if there is no CATCH.
static int find_catch(int *out_pc, int *out_off) {
    int pc = cur_line_idx;
    int depth = 0;
    for (;;) {
        while (tok != T_EOL) {
            if (tok == T_KW && tok_kw == KW_TRY) depth++;
            else if (tok == T_KW && tok_kw == KW_ENDTRY) {
                if (depth == 0) { err("TRY without a matching CATCH"); return 0; }
                depth--;
            } else if (tok == T_KW && tok_kw == KW_CATCH && depth == 0) {
                lex_next();                          // step past CATCH
                *out_pc = pc; *out_off = (int)(tok_start - cur_text);
                return 1;
            }
            lex_next();
        }
        if (++pc >= prog_n) { err("TRY without a matching CATCH"); return 0; }
        lex_at(pc, 0);
    }
}

// TRY : begin a protected block. Locate its CATCH, snapshot the interpreter
// state, push a handler, then carry on executing the block.
static void stmt_try(void) {
    lex_next();                                  // consume TRY
    if (cur_line_idx < 0) { err("TRY can only be used inside a program"); return; }
    if (try_sp >= TRY_MAX) { err("Too many nested TRY blocks"); return; }

    // Remember where the token after TRY starts, so we can resume the block after
    // the forward scan for CATCH has moved the lexer around.
    const char *save_text = cur_text;
    int save_line = cur_line_idx;
    int save_off  = (int)(tok_start - cur_text);

    int cpc, coff;
    if (!find_catch(&cpc, &coff)) return;

    // Re-lex the token right after TRY so the block continues cleanly.
    cur_text = save_text; cur_line_idx = save_line;
    lx = cur_text + save_off;
    lex_next();

    try_rec_t *h = &try_stack[try_sp++];
    h->catch_pc = cpc; h->catch_off = coff;
    h->call_sp = call_sp;
    h->for_sp = for_sp; h->repeat_sp = repeat_sp; h->while_sp = while_sp;
    h->case_sp = case_sp; h->gosub_sp = gosub_sp; h->fn_ret_sp = fn_ret_sp;
    h->local_sp = local_sp;
    h->scratch_base = scratch_base; h->scratch_top = scratch_top;
}

// CATCH reached in normal flow: the protected block finished with no error, so
// discard the handler and skip past the matching ENDTRY (don't run the handler).
static void stmt_catch(void) {
    if (try_sp > 0) try_sp--;                    // this block succeeded
    lex_next();                                  // consume CATCH
    skip_to_close(KW_TRY, KW_ENDTRY, "CATCH without a matching ENDTRY");
}

static void stmt_endtry(void) { lex_next(); }    // no-op join point

// RAISE : throw a user error.
//   RAISE "message"            (code 0)
//   RAISE code                 (numeric code, message = "")
//   RAISE code, "message"
static void stmt_raise(void) {
    lex_next();                                  // consume RAISE
    value_t v = eval_expr();
    if (g_err) return;
    int code = 0;
    char msgbuf[ERRMSG_MAX]; msgbuf[0] = 0;
    if (v.is_str) {
        int n = v.len < ERRMSG_MAX - 1 ? v.len : ERRMSG_MAX - 1;
        for (int i = 0; i < n; i++) msgbuf[i] = v.str[i];
        msgbuf[n] = 0;
    } else {
        code = (int)v.num;
        if (tok == T_COMMA) {
            lex_next();
            value_t m = need_str();
            if (g_err) return;
            int n = m.len < ERRMSG_MAX - 1 ? m.len : ERRMSG_MAX - 1;
            for (int i = 0; i < n; i++) msgbuf[i] = m.str[i];
            msgbuf[n] = 0;
        }
    }
    if (g_err) return;
    // Raise it the same way err() does, but keep the caller-supplied code.
    err(msgbuf[0] ? msgbuf : "User error");
    g_errcode = code;
}

// If an error is pending and the innermost TRY handler belongs to the current
// PROC/FN frame, unwind to it: restore the saved stacks/locals, clear the error,
// and report the CATCH position to resume at (via *pc/*off). Returns 1 if caught.
static int try_handle_error(int *pc, int *off) {
    if (!g_err || try_sp <= 0) return 0;
    try_rec_t *h = &try_stack[try_sp - 1];
    if (h->call_sp != call_sp) return 0;         // handler is in an outer frame: keep unwinding

    for_sp = h->for_sp; repeat_sp = h->repeat_sp; while_sp = h->while_sp;
    case_sp = h->case_sp; gosub_sp = h->gosub_sp; fn_ret_sp = h->fn_ret_sp;
    while (local_sp > h->local_sp) {             // undo LOCALs bound inside the block
        local_sp--;
        *local_stack[local_sp].slot = local_stack[local_sp].old;
    }
    scratch_base = h->scratch_base; scratch_top = h->scratch_top;

    *pc = h->catch_pc; *off = h->catch_off;
    try_sp--;                                    // this handler is now consumed
    g_err = 0;                                   // handled: resume normal flow
    g_branch = 0; g_return = 0;
    return 1;
}

static void exec_statement(void) {
    scratch_reset();                             // reclaim previous temporaries
    if (tok == T_LABEL) lex_next();              // ".name": a branch label, then run the rest
    if (tok == T_EOL || tok == T_COLON) return;
    if (tok == T_KW) {
        if (is_seed_kw(tok_kw)) { exec_seed_keyword(tok_kw); return; }   // seed keyword as a command
        switch (tok_kw) {
            case KW_PRINT: stmt_print();   return;
            case KW_LET:   stmt_let(1);    return;
            case KW_INPUT: stmt_input();   return;
            case KW_DIM:   stmt_dim();     return;
            case KW_GOTO:  stmt_goto();    return;
            case KW_GOSUB: stmt_gosub();   return;
            case KW_RETURN:stmt_return();  return;
            case KW_FOR:   stmt_for();     return;
            case KW_NEXT:  stmt_next();    return;
            case KW_IF:    stmt_if();      return;
            case KW_REPEAT:stmt_repeat();  return;
            case KW_UNTIL: stmt_until();   return;
            case KW_WHILE: stmt_while();   return;
            case KW_ENDWHILE: stmt_endwhile(); return;
            case KW_CASE:  stmt_case();    return;
            case KW_WHEN:
            case KW_OTHERWISE: case_skip_to_end(); return;  // chosen clause finished
            case KW_ENDCASE: stmt_endcase(); return;
            case KW_EXIT:     stmt_exit();     return;
            case KW_CONTINUE: stmt_continue(); return;
            case KW_TRY:      stmt_try();      return;
            case KW_CATCH:    stmt_catch();    return;
            case KW_ENDTRY:   stmt_endtry();   return;
            case KW_RAISE:    stmt_raise();    return;
            case KW_ENDIF: lex_next();     return;   // block-IF join point: no-op
            case KW_ELSE:  stmt_else_block(); return;   // end of a block-IF THEN branch
            case KW_CLS:   lex_next(); con_cls();   return;
            case KW_COLOUR:stmt_colour();  return;
            case KW_SOUND: stmt_sound();   return;
            case KW_TONE:  stmt_tone();    return;
            case KW_POKE:  stmt_poke_kw(); return;
            case KW_EXEC:  stmt_exec();    return;
            case KW_PINMODE: stmt_pinmode(); return;
            case KW_PIN:     stmt_pin();     return;   // write form; read form PIN(n) in expressions
            case KW_PINSET:  stmt_pinset(1); return;
            case KW_PINCLR:  stmt_pinset(0); return;
            case KW_I2CWRITE: stmt_i2cwrite(); return;
            case KW_I2CREAD:  stmt_i2cread();  return;
            case KW_SCREEN: stmt_screen(); return;
            case KW_WAIT:   stmt_wait();   return;
            case KW_DELAY:  stmt_delay();  return;
            case KW_KEYBOARD: stmt_keyboard(); return;
            case KW_MODE:  stmt_mode();    return;
            case KW_GCOL:  stmt_gcol();    return;
            case KW_PLOT:  stmt_plot();    return;
            case KW_MOVE:  stmt_move_draw(4); return;   // MOVE = PLOT 4
            case KW_DRAW:  stmt_move_draw(5); return;   // DRAW = PLOT 5
            case KW_CLG:   lex_next(); con_clg();   return;
            case KW_LINE:      stmt_line();      return;
            case KW_RECTANGLE: stmt_rectangle(); return;
            case KW_CIRCLE:    stmt_circle();    return;
            case KW_ELLIPSE:   stmt_ellipse();   return;
            case KW_FILL:      stmt_fill();      return;
            case KW_GGET:      stmt_gget();      return;
            case KW_GPUT:      stmt_gput();      return;
            case KW_BUFFER:    stmt_buffer();    return;
            case KW_FLIP:      stmt_flip();      return;
            case KW_GTINT:     stmt_gtint();     return;
            case KW_NEWSPRITE:    stmt_newsprite();    return;
            case KW_SPRITETARGET: stmt_spritetarget(); return;
            case KW_TILEMAP:      stmt_tilemap();      return;
            case KW_FONT:         stmt_font();         return;
            case KW_FONTSIZE:     stmt_fontsize();     return;
            case KW_FONTSTYLE:    stmt_fontstyle();    return;
            case KW_GTEXT:        stmt_gtext();        return;
            case KW_DICTSET:  stmt_dictset();  return;
            case KW_DICTDEL:  stmt_dictdel();  return;
            case KW_PUSH:     stmt_push();     return;
            case KW_LISTSET:  stmt_listset();  return;
            case KW_LISTINS:  stmt_listins();  return;
            case KW_LISTDEL:  stmt_listdel();  return;
            case KW_TREESET:  stmt_treeset();  return;
            case KW_TREEDEL:  stmt_treedel();  return;
            case KW_SAVESPRITE: stmt_savesprite(); return;
            case KW_SAVE:  stmt_save();    return;
            case KW_LOAD:  stmt_load();    return;
            case KW_DELETE:stmt_delete();  return;
            case KW_SEED:  stmt_seed();    return;
            case KW_CALL:  stmt_call();    return;
            case KW_CAT:
            case KW_DIR:   stmt_cat(); return;
            case KW_MKDIR: stmt_mkdir(); return;
            case KW_CD:    stmt_cd();    return;
            case KW_RMDIR: stmt_rmdir(); return;
            case KW_PWD:   stmt_pwd();   return;
            case KW_PROC:  call_proc(0, 0); return;
            case KW_ENDPROC: g_return = 1; return;
            case KW_LOCAL: stmt_local();   return;
            case KW_TYPE:  stmt_type();    return;
            case KW_ENDTYPE: err("ENDTYPE without a matching TYPE"); return;
            case KW_DEF:   tok = T_EOL;    return;   // DEF lines are skipped in normal flow
            case KW_IMPORT: tok = T_EOL;   return;   // resolved in the pre-pass; a no-op here
            case KW_REM:   tok = T_EOL;    return;   // ignore rest of line
            case KW_END:                             // END fn/proc = return; bare END = stop
                lex_next();
                if (tok == T_KW && (tok_kw == KW_FN || tok_kw == KW_PROC)) {
                    g_return = 1; return;
                }
                g_stop = 1; return;
            case KW_ON:      stmt_on();      return;
            case KW_READ:    stmt_read();    return;
            case KW_RESTORE: stmt_restore(); return;
            case KW_VDU:     stmt_vdu();     return;
            case KW_DATA:    tok = T_EOL;    return;   // DATA lines are skipped in normal flow
            case KW_STOP:    con_puts("\nSTOP");
                             if (g_runline >= 0) { con_puts(" at line "); con_putn(g_runline); }
                             con_putc('\n'); g_stop = 1; return;
            case KW_TIME: {                            // TIME = expr  (set the centisecond clock)
                lex_next();
                if (tok != T_EQ) { err("Expected '='"); return; }
                lex_next();
                double v = need_num();
                if (g_err) return;
                time_base = (long long)(con_micros() / 10000ULL) - (long long)v;
                return;
            }
            // RUN clears variables (CLR) then runs from the top. It nests another
            // exec_text() sharing the global lexer, so force EOL afterwards.
            case KW_RUN:   lex_next();
                           clear_vars();
                           run_program(0, 0); tok = T_EOL; return;
            case KW_MOUSE:    stmt_mouse();    return;
            case KW_BPUT:     stmt_bput();     return;
            case KW_CLOSE:    stmt_close();    return;
            case KW_PTR: {                             // PTR# ch = expr : set file pointer
                lex_next();                            // consume PTR
                int ch = read_channel();
                if (g_err) return;
                if (tok != T_EQ) { err("Expected '='"); return; }
                lex_next();
                long p = (long)need_num();
                if (g_err) return;
                stg_seek(ch, p);
                return;
            }
            case KW_LIST:     stmt_list();     return;
            case KW_RENUMBER: stmt_renumber(); return;
            case KW_AUTO:     stmt_auto();     return;
            case KW_EDIT:     stmt_edit();     return;
            case KW_NEW:   lex_next(); prog_n = 0; clear_vars();
                           for (int i = 0; i < SEED_MAX; i++) seed_loaded[i] = 0;
                           seed_heap_reset(); coll_reset();
                           for_sp = 0; gosub_sp = 0; return;
            default:       err("That keyword can't be used as a command");  return;
        }
    }
    if (tok == T_VAR) { stmt_let(0); return; }   // implicit assignment
    if (tok == T_QUERY || tok == T_PLING || tok == T_DOLLAR) { stmt_poke(); return; }
    if (tok == T_EQ) {                           // =<expr> : return a value from an FN
        lex_next();
        value_t v = eval_expr();
        if (g_err) return;
        fn_retval = v;
        g_return = 1;
        return;
    }
    err("Expected a command");
}

// Execute one line of source starting at byte offset `off` (which may hold
// several ':'-separated statements).
static void exec_text(const char *text, int off) {
    cur_text = text;
    lx = text + off;
    lex_next();
    while (tok == T_COLON) lex_next();   // skip leading ':' (single-line PROC body)
    while (!g_err && !g_stop && !g_branch && !g_return && tok != T_EOL) {
        exec_statement();
        if (g_err || g_stop || g_branch || g_return) break;
        if (tok == T_COLON) { lex_next(); continue; }
        if (tok == T_EOL)   break;
        err("Unexpected text after the statement");    // junk after a statement
        break;
    }
}

// Build the PROC/FN definition table by scanning the program for "DEF" lines.
static void scan_defs(void) {
    def_n = 0;
    for (int i = 0; i < prog_n && def_n < DEF_MAX; i++) {
        cur_text = prog[i].text;
        lx = prog[i].text;
        lex_next();
        if (tok != T_KW || tok_kw != KW_DEF) continue;
        lex_next();
        if (tok == T_KW && (tok_kw == KW_PROC || tok_kw == KW_FN)) {
            int is_fn = (tok_kw == KW_FN);
            int newstyle = 0;
            if (tok_var[0] == 0) {                // spaced: "DEF fn NAME" / "DEF proc NAME"
                newstyle = 1;
                lex_next();                       // the name follows as its own word
                if (tok != T_VAR) continue;       // malformed DEF line
            }
            s_copy(defs[def_n].name, tok_var, NAME_LEN);
            defs[def_n].is_fn    = is_fn;
            defs[def_n].newstyle = newstyle;
            defs[def_n].line     = i;
            defs[def_n].off      = cur_off();     // just after the name ('(' or body)
            def_n++;
        }
    }
}

// Run a registered event handler PROC (parameterless). call_named parses its
// argument list from the current token, so force T_EOL first to bind zero args;
// call_named saves and restores the rest of the caller's lexer state itself.
static void dispatch_handler(const char *name) {
    tok = T_EOL;
    call_named(0, name, 0);
}

// Check every armed event source and run its handler if it has fired. Called at
// statement/line boundaries from the run loops. `in_event` blocks re-entry so a
// handler can't itself be interrupted by another event.
static void poll_events(void) {
    if (in_event || g_err || g_stop) return;
    if (!ev_timer.active && !ev_mouse.active && !ev_key.active) {
        int any = 0;
        for (int i = 0; i < EV_PIN_MAX; i++) if (ev_pin[i].active) { any = 1; break; }
        if (!any) return;
    }
    in_event = 1;
    if (ev_timer.active) {
        unsigned long long now = con_micros();
        if ((long long)(now - ev_timer.last_us) >= ev_timer.interval_us) {
            ev_timer.last_us = now;
            dispatch_handler(ev_timer.proc);
        }
    }
    for (int i = 0; i < EV_PIN_MAX && !g_err && !g_stop; i++) {
        if (!ev_pin[i].active) continue;
        // Fire on a hardware-latched edge (catches transient pulses on real
        // hardware) OR a software level change of the requested direction (works
        // everywhere, including QEMU which doesn't model the edge detector).
        int cur = gpio_read(ev_pin[i].pin);
        int latched = gpio_poll_edge(ev_pin[i].pin);
        int e = ev_pin[i].edge, prev = ev_pin[i].level;
        int fire = latched
                || (e == 2 && cur != prev)
                || (e == 1 && prev == 0 && cur == 1)
                || (e == 0 && prev == 1 && cur == 0);
        ev_pin[i].level = cur;
        if (fire) dispatch_handler(ev_pin[i].proc);
    }
    if (ev_mouse.active && !g_err && !g_stop) {
        int x, y, b; con_mouse(&x, &y, &b);
        if (x != ev_mouse.x || y != ev_mouse.y || b != ev_mouse.b) {
            ev_mouse.x = x; ev_mouse.y = y; ev_mouse.b = b;
            dispatch_handler(ev_mouse.proc);
        }
    }
    // A keypress: consume it and hold it in g_pending_key so the handler's GET /
    // INKEY reads the same key. Skip if a key is already waiting to be read.
    if (ev_key.active && !g_err && !g_stop && g_pending_key < 0) {
        int k = con_inkey(0);
        if (k >= 0) { g_pending_key = k; dispatch_handler(ev_key.proc); }
    }
    in_event = 0;
}

// Execute program lines starting at (pc,off) until ENDPROC / =<expr> (g_return),
// END (g_stop), end of program, or an error. Used for PROC and FN bodies.
static void run_body(int pc, int off) {
    g_return = 0;
    while (pc >= 0 && pc < prog_n && !g_err && !g_stop && !g_return) {
        poll_events();                           // fire any events between lines
        if (g_err || g_stop || g_return) break;
        cur_line_idx = pc;
        g_runline = prog[pc].num;
        g_branch = 0;
        exec_text(prog[pc].text, off);
        off = 0;
        if (g_err) {                             // a TRY in this frame can catch it
            int npc, noff;
            if (try_handle_error(&npc, &noff)) { pc = npc; off = noff; continue; }
        }
        if (g_err || g_stop || g_return) break;
        if (g_branch) { pc = g_branch_line; off = g_branch_off; }
        else          { pc++; }
    }
}

static int run_depth;                          // nested run_program depth (for IMPORT)

static void run_program(int start_pc, int start_off) {
    int outer = (run_depth == 0);
    run_depth++;
    // Only the outermost RUN pulls in modules (and strips them at the end), so a
    // program that itself issues RUN doesn't re-import on top of the loaded set.
    if (outer) { main_n = prog_n; import_modules(); }
    g_stop = 0;
    gosub_sp = 0;
    for_sp = 0;
    repeat_sp = 0;
    while_sp = 0;
    case_sp = 0;
    call_sp = 0;
    fn_ret_sp = 0;
    local_sp = 0;
    try_sp = 0;
    g_errcode = 0; g_errmsg[0] = 0;
    scratch_base = 0;
    for (int i = 0; i < SEED_MAX; i++) seed_loaded[i] = 0;   // fresh run reloads its seeds
    seed_heap_reset();                                       // and starts with a clean heap
    coll_reset();                                            // ... including its collections
    sound_reset();                                           // silence any leftover notes
    if (outer) events_reset();                               // cancel any ON TIMER/PIN/MOUSE
    data_pc = 0;
    data_off = -1;
    scan_defs();                                            // now also sees module PROC/FN
    int pc  = start_pc;
    int off = start_off;
    // Top-level flow runs the MAIN program only (indices [0, main_n)); module
    // lines live above main_n and are reached solely through PROC/FN calls, so
    // falling off the end of main must not wander into them.
    while (pc >= 0 && pc < main_n && !g_err && !g_stop) {
        sound_pump();                            // advance any queued/background notes
        poll_events();                           // fire ON TIMER/PIN/MOUSE handlers
        if (g_err || g_stop) break;
        cur_line_idx = pc;
        g_runline = prog[pc].num;
        g_branch = 0;
        g_return = 0;
        exec_text(prog[pc].text, off);
        off = 0;
        if (g_err) {                             // a TRY at the top level can catch it
            int npc, noff;
            if (try_handle_error(&npc, &noff)) { pc = npc; off = noff; continue; }
        }
        if (g_err || g_stop) break;
        if (g_branch) { pc = g_branch_line; off = g_branch_off; }
        else          { pc++; }
    }
    cur_line_idx = -1;
    g_runline = -1;
    g_stop = 0;
    g_branch = 0;
    run_depth--;
    if (outer) {
        // An error deferred by an unmatched TRY (or one that never reached its
        // CATCH) was recorded but not printed; report it now so it isn't lost.
        if (g_err && !g_err_reported) {
            con_putc('\n'); con_puts(g_errmsg);
            g_runline = g_errline; err_tail(); g_runline = -1;
            g_err_reported = 1;
        }
        try_sp = 0;
        prog_n = main_n;                       // strip imported module lines
        con_screen(0, 0);                      // restore the startup resolution (no-op if unchanged)
        con_backbuffer(0);                     // un-buffer so the prompt draws to the visible screen
    }
}

