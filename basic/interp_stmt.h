#ifndef INTERP_STMT_H
#define INTERP_STMT_H

#include "interp_types.h"

/* ==================================================================
 * interp_stmt.c -- Core statements and control flow (assignment, IF, FOR, loops, ...).
 * ================================================================== */

/* -------------------------------------------------------------- globals */

extern int     call_sp;   // PROC/FN recursion depth
extern int       case_sp;
extern int  cur_line_idx;   // prog[] index of that line, or -1 if immediate
extern int      def_n;
extern defrec_t defs[DEF_MAX];
extern var_t  *fn_ret_slot[FN_RET_MAX];
extern int     fn_ret_sp;
extern value_t fn_retval;   // value returned from an FN
extern int       for_sp;
extern for_rec_t for_stack[FOR_MAX];
extern int  g_branch;   // a jump was requested this statement
extern int  g_branch_line;   // target prog[] index
extern int  g_branch_off;   // target byte offset within that line
extern int     g_return;   // ENDPROC / END FN / =<expr> ends the current body
extern int  g_stop;   // END encountered -> stop RUN
extern int       gosub_sp;
extern retaddr_t gosub_stack[GOSUB_MAX];
extern int       repeat_sp;
extern retaddr_t repeat_stack[REPEAT_MAX];
extern try_rec_t try_stack[TRY_MAX];
extern int       while_sp;
extern retaddr_t while_stack[WHILE_MAX];

/* ------------------------------------------------------------ functions */

// The module the executing line belongs to (0 = main program, or immediate mode).
int cur_module (void);

// Is `name` a defined function (either style)? Returns its defs[] index or -1.
// Lets a bare `name(args)` in an expression be recognised as a call.
int find_fn_def (const char *name);

// Cur off.
int cur_off (void);

// Branch to line.
void branch_to_line (int num);

// Find the program line that begins with the label ".name". Labels are matched
// only as the first token of a line. Returns the prog[] index, or -1.
int find_label (const char *name);

// Branch to a label by name (GOTO/GOSUB target).
void branch_to_label (const char *name);

// Reposition the global lexer at (program line index pc, byte offset off) and
// read the first token there. Used by the structured-block forward scanners.
void lex_at (int pc, int off);

// Two string values are equal iff same length and same bytes.
int val_equal (value_t a, value_t b);

// Forward-scan from the current lexer position for the keyword `close_kw` that
// matches an enclosing `open_kw`, honouring nesting. On success sets g_branch to
// the position just after the matched close keyword and returns 1; on running
// off the end of the program raises `msg` and returns 0. Used by WHILE/ENDWHILE.
int skip_to_close (int open_kw, int close_kw, const char *msg);

// Rec put num.
int rec_put_num (int ch, double x);

// Rec put str.
int rec_put_str (int ch, const char *s, int len);

// PRINT# ch, item, item, ... : write each value as a typed record.
void stmt_print_file (void);

// INPUT# ch, var, var, ... : read typed records back into variables, array
// elements or record fields.
void stmt_input_file (void);

// Execute the PRINT statement.
void stmt_print (void);

// Assignment to a record variable: either one field (p.x = v, e(3).x = v) or a
// whole record (q = p, e(3) = p), which copies every field across from another
// record of the same type. The current token is just past the target's name.
void stmt_let_record (const var_t *v);

// Execute the LET statement.
void stmt_let (int had_let);

// Unary indirection poke statement: ?addr = v (byte), !addr = v (word),
// $addr = s$ (CR-terminated string). The address is a primary, so use
// parentheses for an arithmetic address: ?(P%+1) = v  (or write P%?1 = v).
void stmt_poke (void);

// Execute the GOTO statement.
void stmt_goto (void);

// Run ':'-separated statements from the current position until ELSE / EOL or a
// control transfer. Used for the THEN and ELSE clauses of IF.
void exec_clause (void);

// Consume the rest of the current IF line, returning 1 if THEN was the last
// token on it (the block-IF form `IF cond THEN` <newline>). The lexer is left
// at EOL. Used while scanning so a nested single-line IF (whose ELSE belongs to
// itself) is skipped as a whole and never miscounted.
int scan_if_line_is_block (void);

// Forward-scan for the ELSE/ENDIF that matches the current block IF. Nested
// block IFs increment the depth; single-line IFs are skipped whole. With
// else_too set we stop at the first depth-0 ELSE *or* ENDIF (the false branch
// jumps to whichever comes first); otherwise only ENDIF (a finished THEN branch
// jumps past its ELSE block). Branches past the matched keyword. Returns 1.
int skip_if_block (int else_too);

// IF <expr> [THEN] (<line>|stmts) [ELSE (<line>|stmts)]    -- single-line form
// IF <expr> THEN <newline> ... [ELSE ...] ENDIF            -- block form
// THEN is optional in the single-line form; the block form is recognised when
// THEN is the last token on the line.
void stmt_if (void);

// A standalone ELSE statement is only reached after running a block IF's THEN
// branch: skip past the matching ENDIF.
void stmt_else_block (void);

// Execute the REPEAT statement.
void stmt_repeat (void);

// Execute the UNTIL statement.
void stmt_until (void);

// WHILE <expr> ... ENDWHILE : a pre-tested loop. The condition is re-evaluated
// each pass, so ENDWHILE branches back to the WHILE keyword (re-running this).
// To keep the stack balanced, ENDWHILE pops before branching back and each
// WHILE pass pushes exactly once.
void stmt_while (void);

// Execute the ENDWHILE statement.
void stmt_endwhile (void);

// CASE <expr> OF / WHEN <e>[,<e>...] / OTHERWISE / ENDCASE : multi-way select.
// The selector is matched immediately by scanning the WHEN clauses; control then
// jumps into the matching clause (or OTHERWISE, or past ENDCASE if none match).
// Falling through into a later WHEN/OTHERWISE means the chosen clause finished,
// so those jump to ENDCASE.
void stmt_case (void);

// Reached by falling through after a chosen clause's body finished: jump past
// the matching ENDCASE. (Encountered as a statement, not during the CASE scan.)
void case_skip_to_end (void);

// Execute the ENDCASE statement.
void stmt_endcase (void);

// COLOUR n            : text colour (0..7 foreground, 128..135 background)
// COLOUR l, r, g, b   : redefine logical colour l's palette entry to an RGB value
void stmt_colour (void);

// LOCAL var[,var...] : save the named variables' current values (restored when
// the enclosing PROC/FN ends) and reset them to zero/empty.
void stmt_local (void);

// Execute the GOSUB statement.
void stmt_gosub (void);

// Execute the RETURN statement.
void stmt_return (void);

// Execute the FOR statement.
void stmt_for (void);

// Execute the NEXT statement.
void stmt_next (void);

// --- EXIT / CONTINUE : break out of / restart the innermost loop -------------
// Is source position (l1,o1) strictly later than (l2,o2)?
int pos_gt (int l1, int o1, int l2, int o2);

// From the current lexer position, scan forward for the `need`-th loop terminator
// (NEXT/UNTIL/ENDWHILE) at nesting depth 0. If `past`, resume just after that
// terminator's whole statement (EXIT leaves the loop); otherwise resume AT the
// terminator so it executes (CONTINUE runs the loop's test). Sets g_branch.
void loop_break_scan (int need, int past);

// Shared EXIT/CONTINUE core. is_exit: leave the loop (pop its frame, resume after
// its terminator); else CONTINUE (keep the frame, resume at the terminator's test).
// want: 0 = innermost loop of any kind, else LOOP_FOR/REPEAT/WHILE.
void loop_control (int is_exit, int want);

// EXIT [FOR|REPEAT|WHILE] : leave the innermost (or named) loop.
void stmt_exit (void);

// CONTINUE [FOR|REPEAT|WHILE] : jump to the innermost (or named) loop's next test.
void stmt_continue (void);

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
void stmt_type (void);

// DIM <name> AS <type> : give a record variable storage for `nelem` elements.
// The current token is AS; on return it is whatever follows the type name.
int dim_record (const char *name, int nelem);

// Execute the DIM statement.
void stmt_dim (void);

// Execute the INPUT statement.
void stmt_input (void);

// MOUSE x, y, b : read the pointer into three numeric variables at once.
// x/y are raw framebuffer pixels (origin top-left); b is the button bitmask
// (bit0=left, bit1=right, bit2=middle). See also the MOUSEX/MOUSEY/MOUSEB
// functions for reading a single value inside an expression.
void stmt_mouse (void);

#endif /* INTERP_STMT_H */
