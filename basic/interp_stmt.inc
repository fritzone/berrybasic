// ===========================================================================
// BerryBasiC — statements: control flow, PRINT/LET/IF, loops, FOR, DIM, INPUT
//
// This file is a fragment of the interpreter and is #included by basic.c.
// It is NOT a standalone translation unit: it shares the single translation
// unit's static state and is compiled only as part of basic.c. Do not add it
// to the build system or compile it on its own.
// ===========================================================================
// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

// Execution position + control flow.
//
// A "position" is a (program-line index, byte offset into that line's text)
// pair, so GOSUB/RETURN and FOR/NEXT can resume in the *middle* of a line.
static int  cur_line_idx;        // prog[] index of that line, or -1 if immediate

// The module the executing line belongs to (0 = main program, or immediate mode).
static int cur_module(void) {
    return (cur_line_idx >= 0 && cur_line_idx < prog_n) ? prog[cur_line_idx].module : 0;
}

static int  g_stop;              // END encountered -> stop RUN
static int  g_branch;            // a jump was requested this statement
static int  g_branch_line;       // target prog[] index
static int  g_branch_off;        // target byte offset within that line

// GOSUB return stack
#define GOSUB_MAX 32
typedef struct { int line; int off; } retaddr_t;
static retaddr_t gosub_stack[GOSUB_MAX];
static int       gosub_sp;

// FOR loop stack
#define FOR_MAX 16
typedef struct {
    char   name[NAME_LEN];
    double limit;
    double step;
    int    line;        // position of the loop body (first thing after FOR...)
    int    off;
} for_rec_t;
static for_rec_t for_stack[FOR_MAX];
static int       for_sp;

// REPEAT loop stack (just the position of the loop body start)
#define REPEAT_MAX 16
static retaddr_t repeat_stack[REPEAT_MAX];
static int       repeat_sp;

// WHILE loop stack: position of the WHILE keyword, so ENDWHILE re-tests it.
#define WHILE_MAX 16
static retaddr_t while_stack[WHILE_MAX];
static int       while_sp;

// CASE nesting depth (the selector is matched immediately, so only the depth
// needs tracking for ENDCASE balancing and nested-CASE skipping).
static int       case_sp;

// Loop-type tags for EXIT/CONTINUE. The three loop stacks (for_/repeat_/while_)
// don't record their relative nesting order, so EXIT/CONTINUE find the innermost
// loop by comparing the source positions of each stack's active frames instead.
enum { LOOP_FOR = 1, LOOP_REPEAT, LOOP_WHILE };

// TRY/CATCH handler stack. Each active TRY records where its CATCH clause starts
// and a snapshot of the interpreter's stacks, so catching an error unwinds any
// loops / PROC frames entered since the TRY and resumes cleanly at the handler.
#define TRY_MAX 16
typedef struct {
    int catch_pc, catch_off;       // where the CATCH handler body begins
    int call_sp;                   // PROC/FN depth the TRY sits at
    int for_sp, repeat_sp, while_sp, case_sp, gosub_sp, fn_ret_sp, local_sp;
    int scratch_base, scratch_top;
} try_rec_t;
static try_rec_t try_stack[TRY_MAX];

// Procedures / functions
static int     g_return;         // ENDPROC / END FN / =<expr> ends the current body
static value_t fn_retval;        // value returned from an FN
static int     call_sp;          // PROC/FN recursion depth

// New-style functions (DEF fn NAME(..) / NAME = expr / END fn) return the value
// of a local variable named after the function. This stack holds that variable
// for each active new-style call, so END fn knows what to hand back.
#define FN_RET_MAX 32
static var_t  *fn_ret_slot[FN_RET_MAX];
static int     fn_ret_sp;

#define DEF_MAX 64
// newstyle: 1 = DEF fn/proc NAME (spaced, END fn/proc-terminated); 0 = glued
// DEF FNNAME / DEF PROCNAME (=<expr> / ENDPROC-terminated).
typedef struct { char name[NAME_LEN]; int is_fn; int newstyle; int line; int off; } defrec_t;
static defrec_t defs[DEF_MAX];
static int      def_n;

// Is `name` a defined function (either style)? Returns its defs[] index or -1.
// Lets a bare `name(args)` in an expression be recognised as a call.
static int find_fn_def(const char *name) {
    for (int i = 0; i < def_n; i++)
        if (defs[i].is_fn && s_eq(defs[i].name, name)) return i;
    return -1;
}

static int cur_off(void) { return (int)(lx - cur_text); }

static void branch_to_line(int num) {
    int idx = find_line_index(num);
    if (idx < 0) { err("No such line number"); return; }
    g_branch = 1; g_branch_line = idx; g_branch_off = 0;
}

// Find the program line that begins with the label ".name". Labels are matched
// only as the first token of a line. Returns the prog[] index, or -1.
static int find_label(const char *name) {
    const char *save_text = cur_text;
    const char *save_lx   = lx;
    int save_tok = tok, save_kw = tok_kw;
    int found = -1;
    int m = cur_module();
    for (int i = 0; i < prog_n; i++) {
        if (prog[i].module != m) continue;
        cur_text = prog[i].text; lx = prog[i].text; lex_next();
        if (tok == T_LABEL && s_eq(tok_var, name)) { found = i; break; }
    }
    cur_text = save_text; lx = save_lx; tok = save_tok; tok_kw = save_kw;
    return found;
}

// Branch to a label by name (GOTO/GOSUB target).
static void branch_to_label(const char *name) {
    int idx = find_label(name);
    if (idx < 0) { err("No such label"); return; }
    g_branch = 1; g_branch_line = idx; g_branch_off = 0;
}

// Reposition the global lexer at (program line index pc, byte offset off) and
// read the first token there. Used by the structured-block forward scanners.
static void lex_at(int pc, int off) {
    cur_text = prog[pc].text;
    lx = cur_text + off;
    lex_next();
}

// Two string values are equal iff same length and same bytes.
static int val_equal(value_t a, value_t b) {
    if (a.is_str != b.is_str) return 0;
    if (a.is_str) {
        if (a.len != b.len) return 0;
        for (int i = 0; i < a.len; i++) if (a.str[i] != b.str[i]) return 0;
        return 1;
    }
    return a.num == b.num;
}

// Forward-scan from the current lexer position for the keyword `close_kw` that
// matches an enclosing `open_kw`, honouring nesting. On success sets g_branch to
// the position just after the matched close keyword and returns 1; on running
// off the end of the program raises `msg` and returns 0. Used by WHILE/ENDWHILE.
static int skip_to_close(int open_kw, int close_kw, const char *msg) {
    int pc = cur_line_idx;
    int depth = 0;
    for (;;) {
        while (tok != T_EOL) {
            if (tok == T_KW && tok_kw == open_kw) depth++;
            else if (tok == T_KW && tok_kw == close_kw) {
                if (depth == 0) { g_branch = 1; g_branch_line = pc; g_branch_off = cur_off(); return 1; }
                depth--;
            }
            lex_next();
        }
        if (++pc >= prog_n) { err(msg); return 0; }
        lex_at(pc, 0);
    }
}

static void exec_statement(void);   // forward decl (IF runs a sub-statement)
static void exec_text(const char *text, int off);   // forward decl (EXEC runs a string)

#define PRINT_FIELD 8     // column width for the ',' separator and TAB alignment

// PRINT items separated by ; (close up), , (next field), ' (newline), TAB(n),
// SPC(n). Column is tracked from the start of this PRINT (assumed column 0).
// --- typed record I/O (PRINT# / INPUT#) -------------------------------------
// Each item written by PRINT# is a self-describing record:
//   number : 0x40, then 8 bytes IEEE-754 double, little-endian
//   string : 0x00, then a 1-byte length (0..255), then that many bytes
// INPUT# reads the tag and reconstructs the value into the target variable.
#define REC_NUM  0x40
#define REC_STR  0x00

static int rec_put_num(int ch, double x) {
    union { double d; unsigned char b[8]; } u; u.d = x;
    if (stg_putb(ch, REC_NUM) < 0) return -1;
    for (int i = 0; i < 8; i++) if (stg_putb(ch, u.b[i]) < 0) return -1;
    return 0;
}
static int rec_put_str(int ch, const char *s, int len) {
    if (len < 0) len = 0;
    if (len > 255) len = 255;
    if (stg_putb(ch, REC_STR) < 0) return -1;
    if (stg_putb(ch, len & 0xFF) < 0) return -1;
    for (int i = 0; i < len; i++) if (stg_putb(ch, (unsigned char)s[i]) < 0) return -1;
    return 0;
}

// PRINT# ch, item, item, ... : write each value as a typed record.
static void stmt_print_file(void) {
    int ch = read_channel();
    if (g_err) return;
    while (tok == T_COMMA || tok == T_SEMI) {
        lex_next();
        if (tok == T_EOL || tok == T_COLON) break;      // trailing separator
        value_t v = eval_expr();
        if (g_err) return;
        int r = v.is_str ? rec_put_str(ch, v.str, v.len) : rec_put_num(ch, v.num);
        if (r < 0) { err("File write error"); return; }
    }
}

// INPUT# ch, var, var, ... : read typed records back into variables, array
// elements or record fields.
static void stmt_input_file(void) {
    int ch = read_channel();
    if (g_err) return;
    while (tok == T_COMMA || tok == T_SEMI) {
        lex_next();
        target_t t;
        if (!parse_target(&t)) return;

        int tag = stg_getb(ch);
        if (tag < 0) { err("End of file"); return; }
        if (tag == REC_NUM) {
            if (t.is_str) { err("Type mismatch: file record is a number"); return; }
            union { double d; unsigned char b[8]; } u;
            for (int i = 0; i < 8; i++) {
                int by = stg_getb(ch);
                if (by < 0) { err("End of file"); return; }
                u.b[i] = (unsigned char)by;
            }
            target_put_num(&t, u.d);
        } else if (tag == REC_STR) {
            if (!t.is_str) { err("Type mismatch: file record is a string"); return; }
            int len = stg_getb(ch);
            if (len < 0) { err("End of file"); return; }
            static char sb[MAX_STR];
            for (int i = 0; i < len; i++) {
                int by = stg_getb(ch);
                if (by < 0) { err("End of file"); return; }
                sb[i] = (char)by;
            }
            target_put_str(&t, sb, len);
        } else {
            err("File is not a PRINT# record");
            return;
        }
        if (g_err) return;
    }
}

static void stmt_print(void) {
    lex_next();                              // consume PRINT
    if (tok == T_HASH) { stmt_print_file(); return; }
    // PRINT USING tmpl$; a; b; ...  — format every numeric item with the same
    // template (as FORMAT$ would); strings, TAB/SPC and separators are unchanged.
    char using_tmpl[MAX_STR + 1]; int using_len = -1;   // -1 = no template active
    if (tok == T_KW && tok_kw == KW_USING) {
        lex_next();
        value_t t = need_str(); if (g_err) return;
        if (tok == T_SEMI || tok == T_COMMA) lex_next();   // separator before the first item
        using_len = t.len > MAX_STR ? MAX_STR : t.len;
        for (int i = 0; i < using_len; i++) using_tmpl[i] = t.str[i];
    }
    int col = 0;
    int trailing_sep = 0;                    // line ended with a separator -> no newline
    while (tok != T_EOL && tok != T_COLON && !(tok == T_KW && tok_kw == KW_ELSE)) {
        if (tok == T_SEMI) { lex_next(); trailing_sep = 1; continue; }
        if (tok == T_SQUOTE) { con_putc('\n'); col = 0; lex_next(); trailing_sep = 1; continue; }
        if (tok == T_COMMA) {                // advance to the next field boundary
            do { con_putc(' '); col++; } while (col % PRINT_FIELD);
            lex_next(); trailing_sep = 1; continue;
        }
        if (tok == T_KW && tok_kw == KW_TAB) {
            lex_next(); if (!expect(T_LP)) return;
            int n = (int)need_num(); if (g_err) return;
            if (!expect(T_RP)) return;
            while (col < n) { con_putc(' '); col++; }
            trailing_sep = 1; continue;
        }
        if (tok == T_KW && tok_kw == KW_SPC) {
            lex_next(); if (!expect(T_LP)) return;
            int n = (int)need_num(); if (g_err) return;
            if (!expect(T_RP)) return;
            for (int i = 0; i < n; i++) { con_putc(' '); col++; }
            trailing_sep = 1; continue;
        }
        value_t v = eval_expr();
        if (g_err) return;
        if (v.is_str) { con_putsn(v.str, v.len); col += v.len; }
        else if (using_len >= 0) {           // PRINT USING: format via the template
            char b[MAX_STR + 1];
            int n = fmt_number(b, using_tmpl, using_len, v.num);
            if (n < 0) { err("Bad format template"); return; }
            con_putsn(b, n); col += n;
        }
        else { char b[40]; int n = dbl_to_str(b, v.num); con_putsn(b, n); col += n; }
        trailing_sep = 0;
    }
    if (!trailing_sep) con_putc('\n');
}

// Assignment to a record variable: either one field (p.x = v, e(3).x = v) or a
// whole record (q = p, e(3) = p), which copies every field across from another
// record of the same type. The current token is just past the target's name.
static void stmt_let_record(const var_t *v) {
    int e;
    if (!rec_index(v, &e)) return;
    const type_t *ty = &types[v->rtype];

    if (tok == T_LABEL) {                        // one field
        int f = type_field_in(ty, tok_var);
        if (f < 0) { err("No such field"); return; }
        lex_next();
        if (tok != T_EQ) { err("Expected '='"); return; }
        lex_next();
        value_t val = eval_expr();
        if (g_err) return;
        if (ty->fis_str[f]) {
            if (!val.is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
            str_store_to(&rec_strs[rec_str_slot(v, e, f)], val.str, val.len);
        } else {
            if (val.is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
            rec_nums[rec_num_slot(v, e, f)] = trunc_int(ty->fis_int[f], val.num);
        }
        return;
    }

    // Whole-record copy: the right-hand side must be another record reference.
    if (tok != T_EQ) { err("Expected a field name like p.x"); return; }
    lex_next();
    if (tok != T_VAR) { err("Expected a record to copy from"); return; }
    const var_t *src = var_lookup(tok_var);
    if (!src || !src->is_rec) { err("Expected a record to copy from"); return; }
    lex_next();
    int se;
    if (!rec_index(src, &se)) return;
    if (tok == T_LABEL) { err("Type mismatch: numbers and text can't be mixed"); return; }
    if (src->rtype != v->rtype) { err("Those records are different types"); return; }
    for (int f = 0; f < ty->nf; f++) {
        if (ty->fis_str[f]) {
            // str_store_to stages the bytes before it allocates, so copying
            // straight out of the source descriptor survives a GC here.
            const strdesc_t *s = &rec_strs[rec_str_slot(src, se, f)];
            str_store_to(&rec_strs[rec_str_slot(v, e, f)], s->sptr, s->slen);
            if (g_err) return;
        } else {
            rec_nums[rec_num_slot(v, e, f)] = rec_nums[rec_num_slot(src, se, f)];
        }
    }
}

static void stmt_let(int had_let) {
    if (had_let) lex_next();                 // consume LET
    if (tok != T_VAR) { err("Expected a variable name"); return; }
    char name[NAME_LEN];
    s_copy(name, tok_var, NAME_LEN);
    int isstr = name_is_str(name);
    var_t *rec = var_lookup(name);           // a record shadows the array/scalar paths
    if (rec && !rec->is_rec) rec = 0;
    lex_next();
    if (rec) { stmt_let_record(rec); return; }

    if (tok == T_LP) {                       // array element assignment
        int idx; arr_t *a;
        if (!arr_elem(name, isstr, &idx, &a)) return;
        if (tok != T_EQ) { err("Expected '='"); return; }
        lex_next();
        value_t v = eval_expr();
        if (g_err) return;
        if (a->is_str) {
            if (!v.is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
            str_store_to(&arr_strs[idx], v.str, v.len);
        } else {
            if (v.is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
            arr_nums[idx] = trunc_int(a->is_int, v.num);
        }
        return;
    }

    if (tok == T_QUERY || tok == T_PLING) {  // poke: P%?off = v (byte) / P%!off = v (word)
        if (isstr) { err("Type mismatch: numbers and text can't be mixed"); return; }
        int op = tok;
        var_t *bv = var_find(name);
        long base = bv ? (long)bv->num : 0;
        lex_next();
        value_t off = prim_base();           // offset is a primary (tight)
        if (g_err) return;
        if (off.is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
        long addr = base + (long)off.num;
        if (tok != T_EQ) { err("Expected '='"); return; }
        lex_next();
        double v = need_num();
        if (g_err) return;
        if (op == T_QUERY) mem_pokeb(addr, (long)v); else mem_pokew(addr, (long)v);
        return;
    }

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

// Unary indirection poke statement: ?addr = v (byte), !addr = v (word),
// $addr = s$ (CR-terminated string). The address is a primary, so use
// parentheses for an arithmetic address: ?(P%+1) = v  (or write P%?1 = v).
static void stmt_poke(void) {
    int op = tok;                            // T_QUERY / T_PLING / T_DOLLAR
    lex_next();
    value_t addrv = prim_base();
    if (g_err) return;
    if (addrv.is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
    long addr = (long)addrv.num;
    if (tok != T_EQ) { err("Expected '='"); return; }
    lex_next();
    if (op == T_DOLLAR) {
        value_t s = need_str();
        if (g_err) return;
        mem_write_str(addr, s.str, s.len);
    } else {
        double v = need_num();
        if (g_err) return;
        if (op == T_QUERY) mem_pokeb(addr, (long)v); else mem_pokew(addr, (long)v);
    }
}

static void stmt_goto(void) {
    lex_next();
    // A label target may be written bare (GOTO start) or dotted (GOTO .start).
    if (tok == T_LABEL || tok == T_VAR) { char nm[NAME_LEN]; s_copy(nm, tok_var, NAME_LEN);
                          lex_next(); branch_to_label(nm); return; }
    if (tok != T_NUM) { err("Expected a line number or label"); return; }
    int target = (int)tok_num;
    lex_next();
    branch_to_line(target);
}

// Run ':'-separated statements from the current position until ELSE / EOL or a
// control transfer. Used for the THEN and ELSE clauses of IF.
static void exec_clause(void) {
    while (!g_err && !g_stop && !g_branch && tok != T_EOL &&
           !(tok == T_KW && tok_kw == KW_ELSE)) {
        exec_statement();
        if (g_err || g_stop || g_branch) return;
        if (tok == T_COLON) { lex_next(); continue; }
        break;
    }
}

// Consume the rest of the current IF line, returning 1 if THEN was the last
// token on it (the block-IF form `IF cond THEN` <newline>). The lexer is left
// at EOL. Used while scanning so a nested single-line IF (whose ELSE belongs to
// itself) is skipped as a whole and never miscounted.
static int scan_if_line_is_block(void) {
    int last_then = 0;
    lex_next();                              // consume IF
    while (tok != T_EOL) {
        last_then = (tok == T_KW && tok_kw == KW_THEN);
        lex_next();
    }
    return last_then;
}

// Forward-scan for the ELSE/ENDIF that matches the current block IF. Nested
// block IFs increment the depth; single-line IFs are skipped whole. With
// else_too set we stop at the first depth-0 ELSE *or* ENDIF (the false branch
// jumps to whichever comes first); otherwise only ENDIF (a finished THEN branch
// jumps past its ELSE block). Branches past the matched keyword. Returns 1.
static int skip_if_block(int else_too) {
    int pc = cur_line_idx;
    int depth = 0;
    for (;;) {
        while (tok != T_EOL) {
            if (tok == T_KW && tok_kw == KW_IF) {
                if (scan_if_line_is_block()) depth++;   // consumes to EOL
                break;
            }
            if (tok == T_KW && tok_kw == KW_ENDIF) {
                if (depth == 0) { g_branch = 1; g_branch_line = pc; g_branch_off = cur_off(); return 1; }
                depth--;
            } else if (else_too && depth == 0 && tok == T_KW && tok_kw == KW_ELSE) {
                g_branch = 1; g_branch_line = pc; g_branch_off = cur_off(); return 1;
            }
            lex_next();
        }
        if (++pc >= prog_n) { err("IF without a matching ENDIF"); return 0; }
        lex_at(pc, 0);
    }
}

// IF <expr> [THEN] (<line>|stmts) [ELSE (<line>|stmts)]    -- single-line form
// IF <expr> THEN <newline> ... [ELSE ...] ENDIF            -- block form
// THEN is optional in the single-line form; the block form is recognised when
// THEN is the last token on the line.
static void stmt_if(void) {
    lex_next();                              // consume IF
    value_t cv = eval_expr();
    if (g_err) return;
    long cond = cv.is_str ? (cv.len != 0) : (cv.num != 0);
    int had_then = 0;
    if (tok == T_KW && tok_kw == KW_THEN) { had_then = 1; lex_next(); }

    if (had_then && tok == T_EOL) {          // block IF (THEN ends the line)
        if (cur_line_idx < 0) { err("Block IF can only be used inside a program"); return; }
        if (cond) return;                    // fall through: run the THEN block
        skip_if_block(1);                    // false: jump to ELSE or ENDIF
        return;
    }

    if (cond) {                              // single-line IF
        if (tok == T_NUM) { int t = (int)tok_num; lex_next(); branch_to_line(t); }
        else exec_clause();                  // run THEN clause (stops at ELSE/EOL)
    } else {
        while (tok != T_EOL && !(tok == T_KW && tok_kw == KW_ELSE)) lex_next();  // skip THEN
        if (tok == T_KW && tok_kw == KW_ELSE) {
            lex_next();
            if (tok == T_NUM) { int t = (int)tok_num; lex_next(); branch_to_line(t); }
            else exec_clause();              // run ELSE clause
        }
    }
    if (!g_branch && !g_err && !g_stop) tok = T_EOL;   // discard any leftover ELSE part
}

// A standalone ELSE statement is only reached after running a block IF's THEN
// branch: skip past the matching ENDIF.
static void stmt_else_block(void) {
    lex_next();                              // consume ELSE
    if (cur_line_idx < 0) { err("ELSE without a matching IF"); return; }
    skip_if_block(0);
}

static void stmt_repeat(void) {
    lex_next();                              // consume REPEAT
    if (cur_line_idx < 0) { err("This can only be used inside a program"); return; }
    if (repeat_sp >= REPEAT_MAX) { err("Too many nested REPEAT loops"); return; }
    repeat_stack[repeat_sp].line = cur_line_idx;   // loop body starts here
    repeat_stack[repeat_sp].off  = cur_off();
    repeat_sp++;
}

static void stmt_until(void) {
    lex_next();                              // consume UNTIL
    double cond = need_num();
    if (g_err) return;
    if (repeat_sp <= 0) { err("UNTIL without a matching REPEAT"); return; }
    if (cond == 0) {                         // condition false -> loop again
        retaddr_t *r = &repeat_stack[repeat_sp - 1];
        g_branch = 1; g_branch_line = r->line; g_branch_off = r->off;
    } else {
        repeat_sp--;                         // true -> exit the loop
    }
}

// WHILE <expr> ... ENDWHILE : a pre-tested loop. The condition is re-evaluated
// each pass, so ENDWHILE branches back to the WHILE keyword (re-running this).
// To keep the stack balanced, ENDWHILE pops before branching back and each
// WHILE pass pushes exactly once.
static void stmt_while(void) {
    int kw_line = cur_line_idx;
    int kw_off  = (int)(tok_start - cur_text);   // offset of the WHILE keyword
    lex_next();                                  // consume WHILE
    value_t cv = eval_expr();
    if (g_err) return;
    long cond = cv.is_str ? (cv.len != 0) : (cv.num != 0);
    if (cond) {
        if (cur_line_idx < 0) { err("WHILE can only be used inside a program"); return; }
        if (while_sp >= WHILE_MAX) { err("Too many nested WHILE loops"); return; }
        while_stack[while_sp].line = kw_line;
        while_stack[while_sp].off  = kw_off;
        while_sp++;                              // run the loop body
    } else {
        skip_to_close(KW_WHILE, KW_ENDWHILE, "WHILE without a matching ENDWHILE");
    }
}

static void stmt_endwhile(void) {
    if (while_sp <= 0) { err("ENDWHILE without a matching WHILE"); return; }
    while_sp--;
    retaddr_t r = while_stack[while_sp];
    g_branch = 1; g_branch_line = r.line; g_branch_off = r.off;  // back to WHILE
}

// CASE <expr> OF / WHEN <e>[,<e>...] / OTHERWISE / ENDCASE : multi-way select.
// The selector is matched immediately by scanning the WHEN clauses; control then
// jumps into the matching clause (or OTHERWISE, or past ENDCASE if none match).
// Falling through into a later WHEN/OTHERWISE means the chosen clause finished,
// so those jump to ENDCASE.
static void stmt_case(void) {
    lex_next();                                  // consume CASE
    value_t sel = eval_expr();
    if (g_err) return;
    // Copy a string selector somewhere stable: evaluating WHEN expressions reuses
    // scratch and may trigger a GC that relocates heap strings.
    static char selbuf[MAX_STR];
    if (sel.is_str) {
        if (sel.len > MAX_STR) { err("Text string is too long"); return; }
        for (int i = 0; i < sel.len; i++) selbuf[i] = sel.str[i];
        sel.str = selbuf;
    }
    if (tok != T_KW || tok_kw != KW_OF) { err("Expected OF after CASE"); return; }
    if (cur_line_idx < 0) { err("CASE can only be used inside a program"); return; }
    if (case_sp >= 16) { err("Too many nested CASE statements"); return; }
    case_sp++;

    // Scan forward for the matching WHEN/OTHERWISE/ENDCASE.
    int pc = cur_line_idx;
    int depth = 0;
    for (;;) {
        while (tok != T_EOL) {
            if (tok == T_KW && tok_kw == KW_CASE) depth++;
            else if (tok == T_KW && tok_kw == KW_ENDCASE) {
                if (depth == 0) {                // no clause matched, no OTHERWISE
                    case_sp--;
                    g_branch = 1; g_branch_line = pc; g_branch_off = cur_off();
                    return;
                }
                depth--;
            } else if (depth == 0 && tok == T_KW && tok_kw == KW_OTHERWISE) {
                g_branch = 1; g_branch_line = pc; g_branch_off = cur_off();  // run OTHERWISE body
                return;
            } else if (depth == 0 && tok == T_KW && tok_kw == KW_WHEN) {
                lex_next();                      // evaluate this clause's value list
                int matched = 0;
                for (;;) {
                    value_t v = eval_expr();
                    if (g_err) return;
                    if (val_equal(sel, v)) matched = 1;
                    if (tok == T_COMMA) { lex_next(); continue; }
                    break;
                }
                if (matched) {                   // run this clause's body
                    g_branch = 1; g_branch_line = pc; g_branch_off = cur_off();
                    return;
                }
                continue;                        // no match: keep scanning this line
            }
            lex_next();
        }
        if (++pc >= prog_n) { err("CASE without a matching ENDCASE"); case_sp--; return; }
        lex_at(pc, 0);
    }
}

// Reached by falling through after a chosen clause's body finished: jump past
// the matching ENDCASE. (Encountered as a statement, not during the CASE scan.)
static void case_skip_to_end(void) {
    if (case_sp <= 0) { err("WHEN/OTHERWISE without a matching CASE"); return; }
    if (skip_to_close(KW_CASE, KW_ENDCASE, "CASE without a matching ENDCASE"))
        case_sp--;
}

static void stmt_endcase(void) {
    lex_next();                                  // consume ENDCASE
    if (case_sp > 0) case_sp--;
}

// COLOUR n            : text colour (0..7 foreground, 128..135 background)
// COLOUR l, r, g, b   : redefine logical colour l's palette entry to an RGB value
static void stmt_colour(void) {
    lex_next();                              // consume COLOUR
    int l = (int)need_num();
    if (g_err) return;
    if (tok == T_COMMA) {                     // palette redefinition
        lex_next();
        int r = (int)need_num(); if (!expect(T_COMMA)) return;
        int g = (int)need_num(); if (!expect(T_COMMA)) return;
        int b = (int)need_num(); if (g_err) return;
        con_palette(l, r, g, b);
        return;
    }
    con_colour(l);                           // BBC COLOUR n (foreground 0..7)
}

// LOCAL var[,var...] : save the named variables' current values (restored when
// the enclosing PROC/FN ends) and reset them to zero/empty.
static void stmt_local(void) {
    lex_next();                              // consume LOCAL
    for (;;) {
        if (tok != T_VAR) { err("Expected a variable name"); return; }
        if (local_sp >= LOCAL_MAX) { err("Out of memory"); return; }
        var_t *v = var_find(tok_var);
        if (!v) return;
        local_stack[local_sp].slot = v;
        local_stack[local_sp].old  = *v;     // save (descriptor stays a GC root)
        local_sp++;
        v->is_rec = 0;                       // LOCAL always makes a plain variable
        v->num = 0; v->s.sptr = 0; v->s.slen = 0;   // reset for the procedure
        lex_next();
        if (tok == T_COMMA) { lex_next(); continue; }
        break;
    }
}

static void stmt_gosub(void) {
    lex_next();
    char label[NAME_LEN]; int is_label = 0; int target = 0;
    if (tok == T_LABEL || tok == T_VAR) { s_copy(label, tok_var, NAME_LEN); is_label = 1; lex_next(); }
    else if (tok == T_NUM) { target = (int)tok_num; lex_next(); }
    else { err("Expected a line number or label"); return; }
    if (cur_line_idx < 0) { err("This can only be used inside a program"); return; }
    if (gosub_sp >= GOSUB_MAX) { err("Out of memory"); return; }
    gosub_stack[gosub_sp].line = cur_line_idx;   // resume after the GOSUB
    gosub_stack[gosub_sp].off  = cur_off();
    gosub_sp++;
    if (is_label) branch_to_label(label); else branch_to_line(target);
    if (g_err) gosub_sp--;                        // undefined target: undo push
}

static void stmt_return(void) {
    lex_next();
    if (gosub_sp <= 0) { err("RETURN without a matching GOSUB"); return; }
    gosub_sp--;
    g_branch = 1;
    g_branch_line = gosub_stack[gosub_sp].line;
    g_branch_off  = gosub_stack[gosub_sp].off;
}

static void stmt_for(void) {
    lex_next();                              // consume FOR
    if (tok != T_VAR) { err("Expected a variable name"); return; }
    char name[NAME_LEN];
    s_copy(name, tok_var, NAME_LEN);
    lex_next();
    if (tok != T_EQ) { err("Expected '='"); return; }
    lex_next();
    double start = need_num(); if (g_err) return;
    if (tok != T_KW || tok_kw != KW_TO) { err("Expected TO in a FOR statement"); return; }
    lex_next();
    double limit = need_num(); if (g_err) return;
    double step = 1;
    if (tok == T_KW && tok_kw == KW_STEP) {
        lex_next();
        step = need_num(); if (g_err) return;
    }
    var_t *r = var_find(name); if (!r) return;
    if (r->is_str) { err("Type mismatch: numbers and text can't be mixed"); return; }
    r->num = trunc_int(r->is_int, start);

    if (cur_line_idx < 0) { err("This can only be used inside a program"); return; }
    if (for_sp >= FOR_MAX) { err("Out of memory"); return; }
    s_copy(for_stack[for_sp].name, name, NAME_LEN);
    for_stack[for_sp].limit = limit;
    for_stack[for_sp].step  = step;
    for_stack[for_sp].line  = cur_line_idx;   // loop body starts right here
    for_stack[for_sp].off   = cur_off();
    for_sp++;
    // Test happens at NEXT, so the body always runs at least once (MS BASIC).
}

static void stmt_next(void) {
    lex_next();                              // consume NEXT
    char name[NAME_LEN]; name[0] = 0;
    if (tok == T_VAR) { s_copy(name, tok_var, NAME_LEN); lex_next(); }

    if (for_sp <= 0) { err("NEXT without a matching FOR"); return; }
    int idx = for_sp - 1;
    if (name[0]) {                           // NEXT I: pop inner loops to reach I
        while (idx >= 0 && !s_eq(for_stack[idx].name, name)) idx--;
        if (idx < 0) { err("NEXT without a matching FOR"); return; }
        for_sp = idx + 1;
    }

    for_rec_t *f = &for_stack[idx];
    var_t *r = var_find(f->name); if (!r) return;
    r->num = trunc_int(r->is_int, r->num + f->step);
    int cont = (f->step >= 0) ? (r->num <= f->limit) : (r->num >= f->limit);
    if (cont) {
        g_branch = 1; g_branch_line = f->line; g_branch_off = f->off;
    } else {
        for_sp = idx;                        // loop done: pop it
    }
}

// --- EXIT / CONTINUE : break out of / restart the innermost loop -------------
// Is source position (l1,o1) strictly later than (l2,o2)?
static int pos_gt(int l1, int o1, int l2, int o2) {
    return (l1 > l2) || (l1 == l2 && o1 > o2);
}

// From the current lexer position, scan forward for the `need`-th loop terminator
// (NEXT/UNTIL/ENDWHILE) at nesting depth 0. If `past`, resume just after that
// terminator's whole statement (EXIT leaves the loop); otherwise resume AT the
// terminator so it executes (CONTINUE runs the loop's test). Sets g_branch.
static void loop_break_scan(int need, int past) {
    int pc = cur_line_idx;
    int depth = 0;
    for (;;) {
        while (tok != T_EOL) {
            if (tok == T_KW && (tok_kw == KW_FOR || tok_kw == KW_REPEAT || tok_kw == KW_WHILE))
                depth++;
            else if (tok == T_KW && (tok_kw == KW_NEXT || tok_kw == KW_UNTIL || tok_kw == KW_ENDWHILE)) {
                if (depth > 0) depth--;
                else if (--need == 0) {
                    if (!past) {                 // CONTINUE: land on the terminator
                        g_branch = 1; g_branch_line = pc; g_branch_off = (int)(tok_start - cur_text);
                        return;
                    }
                    int ck = tok_kw; lex_next();     // EXIT: step past the terminator statement
                    if (ck == KW_NEXT) { if (tok == T_VAR) lex_next(); }        // NEXT [var]
                    else if (ck == KW_UNTIL) { while (tok != T_EOL && tok != T_COLON) lex_next(); }
                    g_branch = 1; g_branch_line = pc; g_branch_off = (int)(tok_start - cur_text);
                    return;
                }
            }
            lex_next();
        }
        if (++pc >= prog_n) { err("Loop is not closed (missing NEXT/UNTIL/ENDWHILE)"); return; }
        lex_at(pc, 0);
    }
}

// Shared EXIT/CONTINUE core. is_exit: leave the loop (pop its frame, resume after
// its terminator); else CONTINUE (keep the frame, resume at the terminator's test).
// want: 0 = innermost loop of any kind, else LOOP_FOR/REPEAT/WHILE.
static void loop_control(int is_exit, int want) {
    const char *what = is_exit ? "EXIT" : "CONTINUE";
    if (for_sp + repeat_sp + while_sp == 0) { err2(what, " is not inside a loop"); return; }

    int t_type, t_line, t_off;
    if (want == LOOP_FOR) {
        if (for_sp <= 0) { err2(what, " FOR: no FOR loop is active"); return; }
        t_type = LOOP_FOR; t_line = for_stack[for_sp-1].line; t_off = for_stack[for_sp-1].off;
    } else if (want == LOOP_REPEAT) {
        if (repeat_sp <= 0) { err2(what, " REPEAT: no REPEAT loop is active"); return; }
        t_type = LOOP_REPEAT; t_line = repeat_stack[repeat_sp-1].line; t_off = repeat_stack[repeat_sp-1].off;
    } else if (want == LOOP_WHILE) {
        if (while_sp <= 0) { err2(what, " WHILE: no WHILE loop is active"); return; }
        t_type = LOOP_WHILE; t_line = while_stack[while_sp-1].line; t_off = while_stack[while_sp-1].off;
    } else {                                    // innermost loop = latest start position
        t_type = 0; t_line = -1; t_off = -1;
        if (for_sp > 0 && pos_gt(for_stack[for_sp-1].line, for_stack[for_sp-1].off, t_line, t_off)) {
            t_type = LOOP_FOR; t_line = for_stack[for_sp-1].line; t_off = for_stack[for_sp-1].off; }
        if (repeat_sp > 0 && pos_gt(repeat_stack[repeat_sp-1].line, repeat_stack[repeat_sp-1].off, t_line, t_off)) {
            t_type = LOOP_REPEAT; t_line = repeat_stack[repeat_sp-1].line; t_off = repeat_stack[repeat_sp-1].off; }
        if (while_sp > 0 && pos_gt(while_stack[while_sp-1].line, while_stack[while_sp-1].off, t_line, t_off)) {
            t_type = LOOP_WHILE; t_line = while_stack[while_sp-1].line; t_off = while_stack[while_sp-1].off; }
    }

    // Pop every loop nested inside the target (a strictly later start position).
    int inner = 0;
    while (for_sp > 0    && pos_gt(for_stack[for_sp-1].line,       for_stack[for_sp-1].off,       t_line, t_off)) { for_sp--;    inner++; }
    while (repeat_sp > 0 && pos_gt(repeat_stack[repeat_sp-1].line, repeat_stack[repeat_sp-1].off, t_line, t_off)) { repeat_sp--; inner++; }
    while (while_sp > 0  && pos_gt(while_stack[while_sp-1].line,   while_stack[while_sp-1].off,   t_line, t_off)) { while_sp--;  inner++; }
    if (is_exit) {                              // EXIT also discards the target itself
        if      (t_type == LOOP_FOR)    for_sp--;
        else if (t_type == LOOP_REPEAT) repeat_sp--;
        else                            while_sp--;
    }
    loop_break_scan(inner + 1, is_exit);
}

// EXIT [FOR|REPEAT|WHILE] : leave the innermost (or named) loop.
static void stmt_exit(void) {
    lex_next();                                 // consume EXIT
    int want = 0;
    if (tok == T_KW && tok_kw == KW_FOR)         { want = LOOP_FOR;    lex_next(); }
    else if (tok == T_KW && tok_kw == KW_REPEAT) { want = LOOP_REPEAT; lex_next(); }
    else if (tok == T_KW && tok_kw == KW_WHILE)  { want = LOOP_WHILE;  lex_next(); }
    loop_control(1, want);
}

// CONTINUE [FOR|REPEAT|WHILE] : jump to the innermost (or named) loop's next test.
static void stmt_continue(void) {
    lex_next();                                 // consume CONTINUE
    int want = 0;
    if (tok == T_KW && tok_kw == KW_FOR)         { want = LOOP_FOR;    lex_next(); }
    else if (tok == T_KW && tok_kw == KW_REPEAT) { want = LOOP_REPEAT; lex_next(); }
    else if (tok == T_KW && tok_kw == KW_WHILE)  { want = LOOP_WHILE;  lex_next(); }
    loop_control(0, want);
}

// TYPE name : field, field$, ... : ENDTYPE
//
// Defines a record shape. Each field takes its kind from the usual suffix ($
// text, % integer, otherwise floating point). Fields may be separated by ','
// or ':' and the list may run over several lines, so both
//
//   TYPE point : x, y : ENDTYPE          and      TYPE point
//                                                   x, y
//                                                 ENDTYPE
//
// parse here. Like DIM, this is a declaration executed at run time: the
// statement has to run before the type can be used.
static void stmt_type(void) {
    lex_next();                                  // consume TYPE
    char tname[NAME_LEN];
    if (!read_name_word(tname, "Expected a type name")) return;
    if (type_find(tname) >= 0) { err("That type is already defined"); return; }
    if (type_n >= MAX_TYPES) { err("Too many types"); return; }
    type_t *ty = &types[type_n];                 // built in place, committed at the end
    s_copy(ty->name, tname, NAME_LEN);
    ty->nf = 0; ty->n_num = 0; ty->n_str = 0;

    int pc      = cur_line_idx;                  // -1 in immediate mode: no lines to walk
    int crossed = 0;                             // did the field list span lines?
    int endoff  = 0;
    for (;;) {
        while (tok == T_COLON || tok == T_COMMA) lex_next();
        if (tok == T_EOL) {                      // continue on the next program line
            if (pc < 0 || pc + 1 >= prog_n) { err("TYPE without a matching ENDTYPE"); return; }
            lex_at(++pc, 0);
            crossed = 1;
            continue;
        }
        if (tok == T_KW && tok_kw == KW_ENDTYPE) { endoff = cur_off(); break; }
        char fname[NAME_LEN];
        if (!read_name_word(fname, "Expected a field name")) return;
        if (ty->nf >= MAX_FIELDS) { err("Too many fields"); return; }
        if (type_field_in(ty, fname) >= 0) { err("That field is already defined"); return; }
        int f = ty->nf++;
        s_copy(ty->fname[f], fname, NAME_LEN);
        ty->fis_str[f] = name_is_str(fname);
        ty->fis_int[f] = name_is_int(fname);
        ty->fslot[f]   = ty->fis_str[f] ? ty->n_str++ : ty->n_num++;
    }
    if (ty->nf == 0) { err("A type needs at least one field"); return; }
    type_n++;
    // Resume after ENDTYPE: by branching if the list ran on past this line,
    // otherwise just by stepping over it so the rest of the line still runs.
    if (crossed) { g_branch = 1; g_branch_line = pc; g_branch_off = endoff; }
    else         lex_next();
}

// DIM <name> AS <type> : give a record variable storage for `nelem` elements.
// The current token is AS; on return it is whatever follows the type name.
static int dim_record(const char *name, int nelem) {
    lex_next();                                  // consume AS
    char tname[NAME_LEN];
    if (!read_name_word(tname, "Expected a type name")) return 0;
    int t = type_find(tname);
    if (t < 0) { err("No such type"); return 0; }
    if (name_is_str(name) || name_is_int(name)) {
        err("A record variable can't have a $ or % suffix"); return 0;
    }
    if (arr_find(name)) { err("That array is already defined"); return 0; }
    var_t *ex = var_lookup(name);
    if (ex && ex->is_rec) { err("That record is already defined"); return 0; }
    var_t *v = var_find(name);
    if (!v) return 0;
    int on, os;
    if (!rec_alloc(t, nelem, &on, &os)) return 0;
    v->is_rec = 1; v->rtype = t; v->nelem = nelem;
    v->roff_num = on; v->roff_str = os;
    return 1;
}

static void stmt_dim(void) {
    lex_next();                              // consume DIM
    for (;;) {
        if (tok != T_VAR) { err("Expected a variable name"); return; }
        char name[NAME_LEN];
        s_copy(name, tok_var, NAME_LEN);
        int isstr = name_is_str(name);
        lex_next();

        if (tok == T_KW && tok_kw == KW_AS) {    // DIM p AS point: one record
            if (!dim_record(name, 1)) return;
            if (tok == T_COMMA) { lex_next(); continue; }
            break;
        }

        if (tok != T_LP) {                   // DIM name <size>: reserve a byte block
            if (isstr) { err("Reserve memory into a numeric variable"); return; }
            double szd = need_num();         // DIM P% 100 reserves 101 bytes (0..100)
            if (g_err) return;
            long base = dim_reserve((int)szd + 1);
            if (g_err) return;
            var_t *v = var_find(name);
            if (v) v->num = trunc_int(v->is_int, (double)base);
            if (tok == T_COMMA) { lex_next(); continue; }
            break;
        }

        int subs[MAX_DIMS];
        int nsub;
        if (!parse_subscripts(subs, &nsub)) return;

        if (tok == T_KW && tok_kw == KW_AS) {    // DIM e(10) AS point: an array of records
            if (nsub != 1) { err("An array of records has one dimension"); return; }
            if (subs[0] < 0) { err("Array index out of range"); return; }
            if (!dim_record(name, subs[0] + 1)) return;   // DIM e(10) -> elements 0..10
            if (tok == T_COMMA) { lex_next(); continue; }
            break;
        }

        if (arr_find(name)) { err("That array is already defined"); return; }
        int counts[MAX_DIMS];
        for (int i = 0; i < nsub; i++) {
            if (subs[i] < 0) { err("Array index out of range"); return; }
            counts[i] = subs[i] + 1;         // DIM A(10) -> indices 0..10
        }
        if (!arr_create(name, nsub, counts, isstr)) return;

        if (tok == T_COMMA) { lex_next(); continue; }
        break;
    }
}

static void stmt_input(void) {
    lex_next();                              // consume INPUT
    if (tok == T_HASH) { stmt_input_file(); return; }   // INPUT# ch, var, ...
    if (tok == T_STR) {                      // optional prompt:  INPUT "NAME"; A$
        int n = 0; while (tok_str[n]) n++;
        con_putsn(tok_str, n);
        lex_next();
        if (tok == T_SEMI || tok == T_COMMA) lex_next();
    }
    con_puts("? ");

    static char inbuf[LINE_LEN];
    int got = con_getline(inbuf, LINE_LEN);
    if (got < 0) { g_stop = 1; return; }     // host EOF ends the program
    int ip = 0;

    for (;;) {
        target_t t;
        if (!parse_target(&t)) return;        // variable, array element or record field

        while (inbuf[ip] == ' ') ip++;       // one comma-delimited field
        int start = ip;
        while (inbuf[ip] && inbuf[ip] != ',') ip++;
        int end = ip;

        if (t.is_str) target_put_str(&t, inbuf + start, end - start);
        else          target_put_num(&t, parse_double(inbuf + start, end - start, 0));
        if (g_err) return;

        if (inbuf[ip] == ',') ip++;
        if (tok == T_COMMA) { lex_next(); continue; }   // more variables to fill
        break;
    }
}

// MOUSE x, y, b : read the pointer into three numeric variables at once.
// x/y are raw framebuffer pixels (origin top-left); b is the button bitmask
// (bit0=left, bit1=right, bit2=middle). See also the MOUSEX/MOUSEY/MOUSEB
// functions for reading a single value inside an expression.
static void stmt_mouse(void) {
    lex_next();                              // consume MOUSE
    int px, py, pb;
    con_mouse(&px, &py, &pb);
    int vals[3] = { px, py, pb };
    for (int i = 0; i < 3; i++) {
        if (tok != T_VAR) { err("Expected a variable name"); return; }
        char name[NAME_LEN];
        s_copy(name, tok_var, NAME_LEN);
        lex_next();
        var_t *var = var_find(name);
        if (!var) return;
        if (var->is_str) { err("MOUSE needs numeric variables"); return; }
        var->num = (double)vals[i];
        if (i < 2 && !expect(T_COMMA)) return;
    }
}

static void run_program(int start_pc, int start_off);

// Print a program line for LIST with the classic look: keywords are shown in
// UPPERCASE, while variable names, numbers, string literals and REM comments are
// left exactly as the user typed them. This is display-only; the stored text and
// the way the line runs are unchanged.
