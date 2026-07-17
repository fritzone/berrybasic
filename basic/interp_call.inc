// ===========================================================================
// BerryBasiC — PROC / FN call
//
// This file is a fragment of the interpreter and is #included by basic.c.
// It is NOT a standalone translation unit: it shares the single translation
// unit's static state and is compiled only as part of basic.c. Do not add it
// to the build system or compile it on its own.
// ===========================================================================
// ---------------------------------------------------------------------------
// PROC / FN call. `name` is the callee; on entry the current token is at the
// argument list ('(' or, for a no-arg call, whatever follows the name).
// Evaluates arguments, binds them (plus any LOCALs) over the callee's
// parameters, runs the body, then restores the caller's state. A function
// returns its value in *retval: an old-style FN via =<expr>, a new-style
// `DEF fn NAME` via the value of its NAME variable at END fn.
// ---------------------------------------------------------------------------

#define MAX_ARGS 16

// A record passed as an argument. Records are passed *by reference*: only these
// offsets travel into the callee, so its parameter addresses the caller's own
// field storage and writes through to it.
typedef struct { int rtype, nelem, off_num, off_str; } recref_t;

// Look ahead, from just past a record variable's name, to decide whether this
// argument is the whole record (p, e(3)) or an expression reading one of its
// fields (p.x). Only scans tokens - nothing is evaluated, so an index with a
// side effect can't fire twice - and puts the lexer back where it found it.
static int arg_is_whole_record(void) {
    lexstate_t sv;
    lex_save(&sv);
    if (tok == T_LP) {                           // step over the element index
        int depth = 1;
        while (depth > 0) {
            lex_next();
            if (tok == T_EOL) { lex_restore(&sv); return 0; }
            if (tok == T_LP) depth++;
            else if (tok == T_RP) depth--;
        }
        lex_next();                              // whatever follows ')'
    }
    int whole = (tok != T_LABEL);
    lex_restore(&sv);
    return whole;
}

static void call_named(int is_fn, const char *name, value_t *retval) {
    // Evaluate arguments (copy string args into scratch so a later GC can't move them).
    value_t  args[MAX_ARGS];
    recref_t recargs[MAX_ARGS];
    int      is_recarg[MAX_ARGS];
    int argc = 0;
    if (tok == T_LP) {
        lex_next();
        if (tok != T_RP) {
            for (;;) {
                if (argc >= MAX_ARGS) { err("Too many arguments"); return; }
                is_recarg[argc] = 0;
                if (tok == T_VAR) {              // a record argument binds by reference
                    const var_t *rv = var_lookup(tok_var);
                    if (rv && rv->is_rec) {
                        lexstate_t sv;
                        lex_save(&sv);
                        lex_next();              // past the name
                        if (arg_is_whole_record()) {
                            int e;
                            if (!rec_index(rv, &e)) return;
                            const type_t *ty = &types[rv->rtype];
                            recargs[argc].rtype   = rv->rtype;
                            recargs[argc].nelem   = 1;
                            recargs[argc].off_num = rv->roff_num + e * ty->n_num;
                            recargs[argc].off_str = rv->roff_str + e * ty->n_str;
                            is_recarg[argc] = 1;
                            args[argc] = v_num(0);
                            argc++;
                            if (tok == T_COMMA) { lex_next(); continue; }
                            break;
                        }
                        lex_restore(&sv);        // p.x: an ordinary expression after all
                    }
                }
                value_t a = eval_expr();
                if (g_err) return;
                if (a.is_str) a = str_in_scratch(a.str, a.len);
                args[argc++] = a;
                if (tok == T_COMMA) { lex_next(); continue; }
                break;
            }
        }
        if (!expect(T_RP)) return;
    }

    // Locate the definition.
    int d = -1;
    for (int i = 0; i < def_n; i++)
        if (defs[i].is_fn == is_fn && s_eq(defs[i].name, name)) { d = i; break; }
    if (d < 0) { err(is_fn ? "No such FN" : "No such PROC"); return; }
    if (call_sp >= 32) { err("Too many nested PROC calls"); return; }

    // Save the caller's resume state.
    const char *save_lx   = lx;
    const char *save_text = cur_text;
    int save_tok = tok, save_kw = tok_kw, save_line = cur_line_idx;
    int save_br = g_branch, save_brl = g_branch_line, save_bro = g_branch_off;
    int save_runline = g_runline;
    double save_num = tok_num;
    char save_var[NAME_LEN]; s_copy(save_var, tok_var, NAME_LEN);
    int save_sbase = scratch_base, save_stop = scratch_top;
    int local_mark = local_sp;
    int save_try = try_sp;                       // discard any TRY the body leaves open

    scratch_base = scratch_top;                  // body temporaries start above the args
    call_sp++;

    // Position at the definition and bind formal parameters to the arguments.
    cur_text = prog[defs[d].line].text;
    lx = prog[defs[d].line].text + defs[d].off;
    lex_next();
    int pi = 0;
    int ok = 1;
    int body_off = defs[d].off;                  // no param list -> body is here
    if (tok == T_LP) {
        lex_next();
        if (tok != T_RP) {
            for (;;) {
                if (tok != T_VAR) { err("Expected a parameter name"); ok = 0; break; }
                char pname[NAME_LEN]; s_copy(pname, tok_var, NAME_LEN);
                lex_next();
                if (pi >= argc) { err("Wrong number of arguments"); ok = 0; break; }
                var_t *pv = var_find(pname);
                if (!pv) { ok = 0; break; }
                local_stack[local_sp].slot = pv;
                local_stack[local_sp].old  = *pv;
                local_sp++;

                if (tok == T_KW && tok_kw == KW_AS) {     // "r AS point": a record, by reference
                    lex_next();
                    char tname[NAME_LEN];
                    if (!read_name_word(tname, "Expected a type name")) { ok = 0; break; }
                    int t = type_find(tname);
                    if (t < 0) { err("No such type"); ok = 0; break; }
                    if (name_is_str(pname) || name_is_int(pname)) {
                        err("A record variable can't have a $ or % suffix"); ok = 0; break;
                    }
                    if (!is_recarg[pi]) { err("That argument must be a record"); ok = 0; break; }
                    if (recargs[pi].rtype != t) { err("Those records are different types"); ok = 0; break; }
                    pv->is_rec   = 1;
                    pv->rtype    = t;
                    pv->nelem    = recargs[pi].nelem;
                    pv->roff_num = recargs[pi].off_num;
                    pv->roff_str = recargs[pi].off_str;
                    pi++;
                    if (tok == T_COMMA) { lex_next(); continue; }
                    break;
                }
                if (is_recarg[pi]) { err("A record argument needs an AS parameter"); ok = 0; break; }

                value_t a = args[pi++];
                pv->is_rec = 0;                  // the name may have been a record before
                if (name_is_str(pname)) {
                    if (!a.is_str) { err("Type mismatch: numbers and text can't be mixed"); ok = 0; break; }
                    str_store(pv, a.str, a.len);
                } else {
                    if (a.is_str) { err("Type mismatch: numbers and text can't be mixed"); ok = 0; break; }
                    pv->is_str = 0; pv->num = trunc_int(pv->is_int, a.num);
                }
                if (tok == T_COMMA) { lex_next(); continue; }
                break;
            }
        }
        if (ok && tok != T_RP) { err("Expected ')' to close the parameter list"); ok = 0; }
        if (ok) body_off = cur_off();            // lx is just past ')' = body start
    }
    if (ok && pi != argc) { err("Wrong number of arguments"); ok = 0; }

    // New-style functions return the value of a variable named after the
    // function. Bind it as a fresh local (so it is private and restored on
    // exit) and remember it so END fn / the post-body read can find it.
    int pushed_ret = 0;
    if (ok && is_fn && defs[d].newstyle) {
        if (fn_ret_sp >= FN_RET_MAX) { err("Too many nested function calls"); ok = 0; }
        else {
            var_t *rv = var_find(name);
            if (!rv) ok = 0;
            else {
                local_stack[local_sp].slot = rv;
                local_stack[local_sp].old  = *rv;
                local_sp++;
                rv->is_str = name_is_str(name);
                rv->is_rec = 0;
                rv->num = 0; rv->s.sptr = 0; rv->s.slen = 0;
                fn_ret_slot[fn_ret_sp++] = rv;
                pushed_ret = 1;
            }
        }
    }

    if (ok) {
        run_body(defs[d].line, body_off);
        if (is_fn && defs[d].newstyle && !g_err) {    // return = the NAME variable
            var_t *rv = fn_ret_slot[fn_ret_sp - 1];
            fn_retval = rv->is_str ? str_in_scratch(rv->s.sptr, rv->s.slen)
                                   : v_num(rv->num);
        }
        if (is_fn && retval && !g_err) *retval = fn_retval;
    }
    if (pushed_ret) fn_ret_sp--;

    // Restore locals (parameters + any LOCALs declared in the body).
    while (local_sp > local_mark) {
        local_sp--;
        *local_stack[local_sp].slot = local_stack[local_sp].old;
    }

    // Restore the caller's state. Keep scratch_top for FN so the return value
    // (which lives in scratch) survives; rewind it for PROC.
    call_sp--;
    try_sp = save_try;                           // drop TRY handlers opened in the body
    scratch_base = save_sbase;
    if (!is_fn) scratch_top = save_stop;
    g_return = 0;
    lx = save_lx; cur_text = save_text;
    tok = save_tok; tok_kw = save_kw; tok_num = save_num;
    s_copy(tok_var, save_var, NAME_LEN);
    cur_line_idx = save_line; g_runline = save_runline;
    g_branch = save_br; g_branch_line = save_brl; g_branch_off = save_bro;
}

// PROC / FN call written with the keyword prefix (PROCname / FNname). The current
// token is KW_PROC or KW_FN with the name in tok_var; consume it, then delegate.
static void call_proc(int is_fn, value_t *retval) {
    char name[NAME_LEN];
    s_copy(name, tok_var, NAME_LEN);
    lex_next();                                  // consume the PROC/FN token
    call_named(is_fn, name, retval);
}

