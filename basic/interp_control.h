#ifndef INTERP_CONTROL_H
#define INTERP_CONTROL_H

#include "interp_types.h"

/* ==================================================================
 * interp_control.c -- TRY/CATCH, the statement dispatcher, and the program run loop.
 * ================================================================== */

/* -------------------------------------------------------------- globals */

extern int run_depth;   // nested run_program depth (for IMPORT)

/* ------------------------------------------------------------ functions */

// Forward-scan from just after TRY for the matching CATCH (honouring nested
// TRY blocks). Records its position in *pc/*off (the start of the handler body).
// Returns 1 on success; raises and returns 0 if there is no CATCH.
int find_catch (int *out_pc, int *out_off);

// TRY : begin a protected block. Locate its CATCH, snapshot the interpreter
// state, push a handler, then carry on executing the block.
void stmt_try (void);

// CATCH reached in normal flow: the protected block finished with no error, so
// discard the handler and skip past the matching ENDTRY (don't run the handler).
void stmt_catch (void);

// ENDTRY is only reached as a statement as the join point at the end of a CATCH
// handler that actually ran (the no-error path skips past it in stmt_catch). If no
// TRY is lexically open, this is a stray ENDTRY.
void stmt_endtry (void);

// RAISE : throw a user error.
//   RAISE "message"            (code 0)
//   RAISE code                 (numeric code, message = "")
//   RAISE code, "message"
void stmt_raise (void);

// If an error is pending and the innermost TRY handler belongs to the current
// PROC/FN frame, unwind to it: restore the saved stacks/locals, clear the error,
// and report the CATCH position to resume at (via *pc/*off). Returns 1 if caught.
int try_handle_error (int *pc, int *off);

// Exec statement.
void exec_statement (void);

// Execute one line of source starting at byte offset `off` (which may hold
// several ':'-separated statements).
void exec_text (const char *text, int off);

// Build the PROC/FN definition table by scanning the program for "DEF" lines.
void scan_defs (void);

// Run a registered event handler PROC (parameterless). call_named parses its
// argument list from the current token, so force T_EOL first to bind zero args;
// call_named saves and restores the rest of the caller's lexer state itself.
void dispatch_handler (const char *name);

// Check every armed event source and run its handler if it has fired. Called at
// statement/line boundaries from the run loops. `in_event` blocks re-entry so a
// handler can't itself be interrupted by another event.
void poll_events (void);

// Execute program lines starting at (pc,off) until ENDPROC / =<expr> (g_return),
// END (g_stop), end of program, or an error. Used for PROC and FN bodies.
void run_body (int pc, int off);

// Run program once.
void run_program_once (int start_pc, int start_off);

// Drain the CHAIN queue: load and run each queued .BAS in turn. Iterative (not
// recursive) so a menu that endlessly chains example->menu->example never grows
// the C stack. Each program is loaded fresh and run as an outer program, so it
// gets a clean screen, variables and modules - just like a top-level RUN.
void run_chain_queue (void);

// Run program.
void run_program (int start_pc, int start_off);

#endif /* INTERP_CONTROL_H */
