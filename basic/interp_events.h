#ifndef INTERP_EVENTS_H
#define INTERP_EVENTS_H

#include "interp_types.h"

/* ==================================================================
 * interp_events.c -- DATA/READ, the ON TIMER/PIN/MOUSE event machinery, and VDU.
 * ================================================================== */

/* -------------------------------------------------------------- globals */

extern char data_item[LINE_LEN];
extern int  data_off;   // offset of next item in prog[data_pc], -1 = not located
extern int  data_pc;   // prog index to scan for DATA from
extern ev_key_t   ev_key;
extern ev_mouse_t ev_mouse;
extern ev_pin_t   ev_pin[EV_PIN_MAX];
extern ev_timer_t ev_timer;
extern unsigned long long g_frame_us;   // WAIT's ~60 Hz cadence clock
extern int  in_event;   // reentrancy guard while a handler runs

/* ------------------------------------------------------------ functions */

// Reset data to its initial state.
void data_reset (int line_num);

// Line is data.
int line_is_data (const char *t, int *body);

// Fetch the next DATA item into data_item[]; returns 1, or 0 when out of data.
int data_next (int *len);

// Reset events to its initial state.
void events_reset (void);

// Read the handler's PROC name (glued "PROCname" or spaced "PROC name") into out.
int read_handler_proc (char *out);

// Word is.
int word_is (const char *w);

// ON TIMER cs PROC name   |   ON TIMER OFF
void on_timer (void);

// ON MOUSE PROC name   |   ON MOUSE OFF
void on_mouse (void);

// ON KEY PROC name   |   ON KEY OFF
// The handler reads the triggering key with GET / GET$ / INKEY(0), which return
// the very key that fired the event (it is held in g_pending_key).
void on_key (void);

// ON DEBUG PROC name | ON DEBUG OFF : register/clear a BASIC debugger handler.
void on_debug (void);

// ON PIN p [RISING|FALLING] PROC name   |   ON PIN p OFF   |   ON PIN OFF
void on_pin (void);

// WAIT : block until the next ~1/60 s frame boundary, giving an animation loop a
// steady cadence without a busy spin in the program. (There is no hardware vsync
// interrupt on this path; this is frame pacing, not tear-free page flipping.)
void stmt_wait (void);

// DELAY cs : pause for cs centiseconds (the same units as TIME and INKEY), so
// DELAY 50 waits half a second. Unlike an empty counting loop, the duration is
// real-time and independent of CPU speed. Background audio keeps playing during
// the wait; a non-positive value returns at once.
void stmt_delay (void);

// Execute the ON statement.
void stmt_on (void);

// VDU n[,n...][;] : send each value's bytes to the VDU driver. A value followed
// by ';' is sent as a 16-bit word (two bytes, least-significant first); otherwise
// just its least-significant byte is sent. A trailing '|' (BBC shorthand) sends
// nine zero bytes, padding out a VDU 23 command.
void stmt_vdu (void);

// Execute the RESTORE statement.
void stmt_restore (void);

// READ var[,var...] : assign successive DATA items (numeric or string).
void stmt_read (void);

#endif /* INTERP_EVENTS_H */
