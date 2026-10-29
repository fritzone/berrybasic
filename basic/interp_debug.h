#ifndef INTERP_DEBUG_H
#define INTERP_DEBUG_H

#include "interp_types.h"

/* ==================================================================
 * interp_debug.c -- the debugger hook core.
 *
 * A mechanism-only layer the interpreter fires callbacks from at six sites
 * (line, keyword, PROC/FN call & return, variable write, error). When no
 * front-end is attached, dbg_active is 0 and every hook is a single predicted
 * branch, so a program that never touches the debugger runs at full speed.
 *
 * A *front-end* decides what to do when a hook fires. It can be:
 *   - a POD or seed  (dbg_attach a C callback, drive it with the dbg_* services),
 *   - a BASIC PROC   (ON DEBUG PROC handler),
 *   - or nothing     (the built-in console stepper below is the fallback).
 *
 * The dbg_ctx passed to a front-end, and the dbg_* service signatures, live in
 * berry_services.h (shared with PODs); the BERRY_DBG_* event ids come from there.
 * ================================================================== */

/* -------------------------------------------------------------- limits */

#define DBG_MAX_BP    32      // line + PROC breakpoints
#define DBG_MAX_WATCH 16      // watchpoints + SHOW displays
#define DBG_COND_LEN  80      // a breakpoint/watch condition expression
#define DBG_FRAME_MAX 40      // recorded PROC/FN call frames (matches call depth cap)
#define DBG_KW_WORDS  16      // 512-bit keyword bitset (built-in keyword ids only)

/* Interpreter-internal run mode (distinct from the BERRY_DBG_* stop events). */
typedef enum { DBG_RUN = 0, DBG_STEP, DBG_STEP_OVER, DBG_STEP_OUT, DBG_STOPPED } dbg_mode;

/* Breakpoint kinds stored in dbg_bp[]. Keyword breakpoints live in the bitset
 * dbg_bp_kw[] instead, so the per-statement check is one word load. */
enum { BP_LINE = 0, BP_PROC };

typedef struct {
    int  kind;                 // BP_LINE / BP_PROC
    int  line;                 // BP_LINE: user line number
    char proc[NAME_LEN];       // BP_PROC: PROC/FN name
    char cond[DBG_COND_LEN];   // optional condition, "" = unconditional
    int  ignore;               // AFTER n: skip this many hits before stopping
    int  every;                // EVERY n: stop on every nth hit (0 = every hit)
    int  hits;                 // times reached
    int  enabled;
} dbg_bp_t;

/* A watchpoint (break on write) or a SHOW display (print at each stop). */
typedef struct {
    char name[NAME_LEN];
    char cond[DBG_COND_LEN];   // value condition, "" = any write
    int  show_only;            // 1 = SHOW (display, never break)
    int  enabled;
} dbg_watch_t;

/* One recorded PROC/FN frame, maintained by the call/return hooks (backtrace). */
typedef struct { char name[NAME_LEN]; int is_fn; int call_line; } dbg_frame_t;

/* -------------------------------------------------------------- globals */

extern int      dbg_active;            // master switch: 0 = every hook is a no-op
extern dbg_mode dbg_step_mode;
extern int      dbg_step_depth;        // call depth captured for STEP_OVER/OUT
extern int      dbg_break_every_line;  // single-step every line (TRACE ON)
extern int      dbg_step_stmt;         // STEP also stops between ':' statements
extern int      dbg_break_on_error;    // drop into the debugger when the program errors
extern int      dbg_trace_fh;          // open file handle for TRACE TO, or 0
extern int      dbg_trace_calls;       // TRACE TO also logs PROC/FN entry
extern int      dbg_in_hook;           // re-entrancy guard (no hook inside a hook)
extern int      dbg_run_active;        // a RUN started by a debug front-end (keep bps)

extern dbg_ctx  dbg_cur;               // context of the current stop
extern dbg_bp_t dbg_bp[DBG_MAX_BP];
extern int      dbg_bp_n;
extern unsigned dbg_bp_kw[DBG_KW_WORDS];
extern int      dbg_bp_kw_n;           // count of armed keyword bits
extern dbg_watch_t dbg_watch_set[DBG_MAX_WATCH];
extern int      dbg_watch_n;
extern dbg_frame_t dbg_frames[DBG_FRAME_MAX];
extern int      dbg_frame_n;

extern void   (*dbg_on_stop_cb)(const dbg_ctx *);  // C front-end, or 0
extern char     dbg_on_stop_proc[NAME_LEN];        // BASIC front-end PROC, or ""

/* ------------------------------------------------------------ hook sites */

// Fired from the run loops (run_program_once / run_body) before each line.
void dbg_line_hook(int pc, int off);
// Fired from exec_statement before the keyword dispatch switch.
void dbg_kw_hook(int kw);
// Fired around call_named: on entry (push a frame) and return (pop).
void dbg_call_hook(const char *name, int is_fn);
void dbg_ret_hook(const char *name);
// Fired from stmt_let after a scalar write (watchpoints).
void dbg_write_hook(const char *name, int is_str, double num);
// Fired from err() for an error that would otherwise reach the user.
void dbg_error_hook(void);

// Fast per-statement predicate: is a keyword breakpoint armed on `kw`?
int  dbg_kw_armed(int kw);
// Is any watchpoint (not SHOW-only) armed on `name`?
int  dbg_watch_armed(const char *name);

/* ------------------------------------------------------------ mechanism */

// Enter the stopped state: invoke the attached front-end (C callback, BASIC
// PROC, or the built-in console stepper), then apply its resume decision.
void dbg_stop(dbg_ctx *ctx);

// Full clear: forget every breakpoint/watch and detach (called from NEW).
void dbg_reset(void);
// Per-run reset of transient state; breakpoints persist across RUN.
void dbg_run_begin(void);

// Resume decisions. A front-end (or a statement handler) calls exactly one.
void dbg_do_cont(void);
void dbg_do_step(void);
void dbg_do_step_over(void);
void dbg_do_step_out(void);
void dbg_do_abort(void);

// Recompute dbg_active from the armed state (called after any change).
void dbg_refresh_active(void);

// Evaluate a BASIC expression in the current (paused) context. Returns 0 for a
// number (in *num), 1 for a string (copied into str/slen), or -1 on error.
int  dbg_eval_expr(const char *expr, double *num, char *str, int slen);
// A condition is true when it evaluates to a non-zero number.
int  dbg_cond_true(const char *cond);

/* ------------------------------------------------------------ BASIC surface */

void stmt_trace(void);      // TRACE ON|OFF | TRACE TO "file"[,CALLS]
void stmt_break(void);      // BREAK line[ IF e][ EVERY n|AFTER n] | PROC n | KEYWORD "K" | ERROR
void stmt_unbreak(void);    // UNBREAK line | ALL | ERROR
void stmt_watch(void);      // WATCH var[ IF e]
void stmt_show(void);       // SHOW var
void stmt_step(void);       // STEP [OVER|OUT|IN|STMT]
void stmt_cont(void);       // CONT
void stmt_backtrace(void);  // BACKTRACE
void stmt_dbgset(void);     // SET var = expr
void stmt_profile(void);    // PROFILE ON|OFF | PROFILE (report)

// The value of DBGLINE: the stopped line, or 0 when running normally.
int  dbg_line_value(void);

/* ------------------------------------------------------------ services */

void dbg_svc_attach(void (*on_stop)(const dbg_ctx *));
void dbg_svc_detach(void);
int  dbg_svc_break_line(int line);
int  dbg_svc_break_proc(const char *name);
int  dbg_svc_break_kw(const char *keyword);
void dbg_svc_break_every(int on);
void dbg_svc_clear(int line);
int  dbg_svc_list_breaks(int *lines, int max);
int  dbg_svc_watch(const char *var, const char *cond);
void dbg_svc_break_error(int on);
int  dbg_svc_break_line_if(int line, const char *cond, int ignore);
void dbg_svc_trace_to(int fh, int include_calls);
void dbg_svc_trace_off(void);
int  dbg_svc_where(dbg_ctx *out);
int  dbg_svc_set_num(const char *name, double v);
int  dbg_svc_set_str(const char *name, const char *s, int n);
int  dbg_svc_var_count(void);
int  dbg_svc_var_at(int i, char *name, int namelen, int *is_str, int *is_arr);
int  dbg_svc_stack_depth(void);
int  dbg_svc_stack_frame(int i, char *name, int namelen, int *call_line);
int  dbg_svc_line_count(void);
int  dbg_svc_line_at(int idx, int *number, char *text, int textlen);
int  dbg_svc_run(const char *path);

#endif /* INTERP_DEBUG_H */
