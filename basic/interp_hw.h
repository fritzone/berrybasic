#ifndef INTERP_HW_H
#define INTERP_HW_H

#include "interp_types.h"

/* ==================================================================
 * interp_hw.c -- Hardware statements: sound, GPIO and I2C.
 * ================================================================== */

/* -------------------------------------------------------------- globals */

extern int snd_out_freq, snd_out_vol;   // last tone handed to the hardware
extern int snd_out_freq, snd_out_vol;   // last tone handed to the hardware

/* ------------------------------------------------------------ functions */

// Reset sound to its initial state.
void sound_reset (void);

// Sound enqueue.
void sound_enqueue (int chan, int freq, int vol, long int dur_us);

// Advance every channel's playback and push the audible tone to the hardware.
// Public so the backends' idle loops can keep the queue moving.
void sound_pump (void);

// Test / introspection helpers (also drive the host `basic_host` build).
int sound_cur_freq (void);

// Sound cur vol.
int sound_cur_vol (void);

// Sound queued.
int sound_queued (void);

// BBC pitch -> Hz: 4 units = 1 semitone (48 = 1 octave), and pitch 89 is the A
// above middle C (440 Hz), so freq = 440 * 2^((pitch-89)/48).
int pitch_to_hz (int pitch);

// Raise the host guard; returns 1 if GPIO is unavailable (caller should bail).
int gpio_guard (void);

// GPIO helper: pin arg.
int gpio_pin_arg (void);

// PINMODE pin, OUTPUT | INPUT [PULLUP|PULLDOWN] | ALT f
void stmt_pinmode (void);

// PIN pin, level     (statement form; the read form PIN(n) is in eval_function)
void stmt_pin (void);

// PINSET mask / PINCLR mask : atomically set/clear every pin whose bit is 1.
void stmt_pinset (int setit);

// I2C needs a real Pi (QEMU does not model the BSC); raise a clear error otherwise.
int i2c_guard (void);

// I2CWRITE addr, b1 [, b2, ...] : send the listed bytes to the device at `addr`.
void stmt_i2cwrite (void);

// I2CREAD addr, buf, count : read `count` bytes from `addr` into the DIM buffer.
void stmt_i2cread (void);

// Execute the SOUND statement.
void stmt_sound (void);

// TONE frequency_hz, duration_ms [, volume]   (direct, non-BBC helper)
// Plays on channel 0. Volume defaults to full (15).
void stmt_tone (void);

#endif /* INTERP_HW_H */
