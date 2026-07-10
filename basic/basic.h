#ifndef BASIC_H
#define BASIC_H

// Portable BASIC interpreter. Depends only on console.h, so it builds unchanged
// for both the bare-metal target and the host test harness.

void basic_init(void);   // reset program, variables, and state
void basic_repl(void);   // read-eval-print loop (returns on host EOF)

// Sound engine (implemented in basic.c). sound_pump() advances the queued,
// background note player and should be called frequently — the run loop and the
// REPL do, and a backend's key-wait idle loop may call it too so notes keep
// playing while waiting for input.
void sound_pump(void);
int  sound_cur_freq(void);   // currently audible tone in Hz (0 = silent)
int  sound_cur_vol(void);    // currently audible volume 1..15 (0 = silent)
int  sound_queued(void);     // notes still pending/playing across all channels

#endif
