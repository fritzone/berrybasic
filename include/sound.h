#ifndef SOUND_H
#define SOUND_H

// Minimal audio abstraction that the BASIC interpreter talks to. Two backends
// implement it: the bare-metal PWM tone generator on the Raspberry Pi 4's 3.5mm
// analogue jack (drivers/sound.c) and a host stub (host/sound_host.c) used when
// testing the interpreter on Linux.
//
// This is deliberately a single-voice, square-wave contract: the backend plays
// at most one tone at a time and sustains it in hardware until it is changed or
// silenced. All queueing / channel arbitration / note timing lives in the
// portable interpreter (basic.c, the `sound_*` engine), so it is identical on
// every backend and covered by the host unit tests.

// Bring the audio hardware up (configure the PWM clock and route the jack). Safe
// to call more than once; the host backend is a no-op.
void snd_init(void);

// Start (or retune) the single hardware voice to a square wave of `freq_hz` at
// volume `vol` (0..15, larger is louder). A freq_hz <= 0 or vol <= 0 is treated
// as silence.
void snd_set_tone(int freq_hz, int vol);

// Stop the voice (output goes quiet). Equivalent to snd_set_tone(0, 0).
void snd_silence(void);

#endif
