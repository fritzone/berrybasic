// ===========================================================================
// BerryBasiC — hardware statements: sound engine, GPIO, I2C, SOUND/TONE
//
// This file is a fragment of the interpreter and is #included by basic.c.
// It is NOT a standalone translation unit: it shares the single translation
// unit's static state and is compiled only as part of basic.c. Do not add it
// to the build system or compile it on its own.
// ===========================================================================
// --- Sound: a portable, queued, background tone player ----------------------
// The hardware backend (sound.h) plays at most one square-wave tone at a time.
// Everything that makes it feel like BBC SOUND — four independent channels, a
// per-channel note queue, note durations, and "play in the background while the
// program keeps running" — lives here, so it behaves identically on the target
// and the host and is exercised by the unit tests.
//
// Each channel advances on its own clock: at every sound_pump() a channel whose
// current note has expired starts the next queued note. All channels can be
// "sounding" at once, but with a single mono voice only the lowest-numbered
// active channel is actually audible; the rest still count down so they don't
// starve. sound_pump() must be called often (it is: from the run loop, the REPL,
// and the target's key-wait idle loop) — but because the tone is sustained in
// hardware, the current note keeps playing even between pumps.

#define SND_CHANS 4          // BBC channels 0..3
#define SND_QLEN  9          // per-channel ring buffer (8 usable notes)

typedef struct { int freq, vol; long dur_us; } snd_note;   // dur_us < 0 = play forever

static struct {
    snd_note q[SND_QLEN];
    int head, tail;                        // ring buffer indices
    int active;                            // a note is currently sounding
    int forever;                           // the current note has no end time
    int cur_freq, cur_vol;
    unsigned long long end_us;             // when the current note ends
} snd_ch[SND_CHANS];

static int snd_out_freq = -1, snd_out_vol = -1;   // last tone handed to the hardware

static void sound_reset(void) {
    for (int c = 0; c < SND_CHANS; c++) {
        snd_ch[c].head = snd_ch[c].tail = 0;
        snd_ch[c].active = snd_ch[c].forever = 0;
        snd_ch[c].cur_freq = snd_ch[c].cur_vol = 0;
        snd_ch[c].end_us = 0;
    }
    snd_out_freq = snd_out_vol = -1;
    snd_silence();
}

static void sound_enqueue(int chan, int freq, int vol, long dur_us) {
    if (chan < 0 || chan >= SND_CHANS) return;
    int nt = (snd_ch[chan].tail + 1) % SND_QLEN;
    if (nt == snd_ch[chan].head) return;               // queue full: drop the note
    snd_ch[chan].q[snd_ch[chan].tail].freq   = freq;
    snd_ch[chan].q[snd_ch[chan].tail].vol    = vol;
    snd_ch[chan].q[snd_ch[chan].tail].dur_us = dur_us;
    snd_ch[chan].tail = nt;
}

// Advance every channel's playback and push the audible tone to the hardware.
// Public so the backends' idle loops can keep the queue moving.
void sound_pump(void) {
    unsigned long long now = con_micros();
    for (int c = 0; c < SND_CHANS; c++) {
        if (snd_ch[c].active && !snd_ch[c].forever && now >= snd_ch[c].end_us)
            snd_ch[c].active = 0;                        // this note finished
        if (!snd_ch[c].active && snd_ch[c].head != snd_ch[c].tail) {
            snd_note n = snd_ch[c].q[snd_ch[c].head];
            snd_ch[c].head = (snd_ch[c].head + 1) % SND_QLEN;
            snd_ch[c].active   = 1;
            snd_ch[c].forever  = (n.dur_us < 0);
            snd_ch[c].cur_freq = n.freq;
            snd_ch[c].cur_vol  = n.vol;
            snd_ch[c].end_us   = now + (unsigned long long)(n.dur_us < 0 ? 0 : n.dur_us);
        }
    }
    int wf = 0, wv = 0;
    for (int c = 0; c < SND_CHANS; c++)                  // lowest active channel wins
        if (snd_ch[c].active) { wf = snd_ch[c].cur_freq; wv = snd_ch[c].cur_vol; break; }
    if (wf != snd_out_freq || wv != snd_out_vol) {       // only touch hardware on a change
        snd_out_freq = wf; snd_out_vol = wv;
        if (wf > 0 && wv > 0) snd_set_tone(wf, wv);
        else                  snd_silence();
    }
}

// Test / introspection helpers (also drive the host `basic_host` build).
int sound_cur_freq(void) { return snd_out_freq > 0 ? snd_out_freq : 0; }
int sound_cur_vol(void)  { return snd_out_vol  > 0 ? snd_out_vol  : 0; }
int sound_queued(void) {
    int n = 0;
    for (int c = 0; c < SND_CHANS; c++) {
        int cnt = snd_ch[c].tail - snd_ch[c].head;
        if (cnt < 0) cnt += SND_QLEN;
        n += cnt + (snd_ch[c].active ? 1 : 0);
    }
    return n;
}

// BBC pitch -> Hz: 4 units = 1 semitone (48 = 1 octave), and pitch 89 is the A
// above middle C (440 Hz), so freq = 440 * 2^((pitch-89)/48).
static int pitch_to_hz(int pitch) {
    return (int)(440.0 * dpow(2.0, (pitch - 89) / 48.0) + 0.5);
}

// SOUND channel, amplitude, pitch, duration   (BBC-style)
//   channel   0..3
//   amplitude 0 = silent, else loudness; -15..-1 (BBC) and 1..15 both map to 1..15
//   pitch     0..255, quarter-semitone steps (53 = middle C, 89 = A 440)
//   duration  in twentieths of a second; -1 = play until replaced
// SOUND OFF   silences everything and empties the queues.
// --- GPIO statements --------------------------------------------------------
// Numbering is BCM: PIN 17 is BCM GPIO 17, not header pin 17. Valid pins are the
// 40-pin header, BCM 0..27 (avoid 0/1, the HAT ID EEPROM); higher pins drive the
// SD card and system peripherals and are deliberately unreachable. Every verb
// first checks that we are on real hardware (the host build has no header).

// Raise the host guard; returns 1 if GPIO is unavailable (caller should bail).
static int gpio_guard(void) {
    if (!gpio_available()) { err("GPIO needs the Pi, not the host build"); return 1; }
    return 0;
}
static int gpio_pin_arg(void) {          // read+validate a BCM header pin (0..27)
    int p = (int)need_num();
    if (g_err) return -1;
    if (p < 0 || p > 27) { err("No such pin"); return -1; }
    return p;
}

// PINMODE pin, OUTPUT | INPUT [PULLUP|PULLDOWN] | ALT f
static void stmt_pinmode(void) {
    lex_next();                          // consume PINMODE
    if (gpio_guard()) return;
    int pin = gpio_pin_arg(); if (g_err) return;
    if (!expect(T_COMMA)) return;
    if (tok != T_KW) { err("Bad pin mode"); return; }
    int m = tok_kw;
    if (m == KW_OUTPUT) {
        lex_next();
        gpio_set_mode(pin, GPIO_OUT, 0);
        gpio_set_pull(pin, GPIO_PULL_NONE);
    } else if (m == KW_INPUT) {
        lex_next();
        int pull = GPIO_PULL_NONE;
        if (tok == T_KW && tok_kw == KW_PULLUP)        { pull = GPIO_PULL_UP;   lex_next(); }
        else if (tok == T_KW && tok_kw == KW_PULLDOWN) { pull = GPIO_PULL_DOWN; lex_next(); }
        gpio_set_mode(pin, GPIO_IN, 0);
        gpio_set_pull(pin, pull);
    } else if (m == KW_ALT) {
        lex_next();
        int f = (int)need_num(); if (g_err) return;
        if (f < 0 || f > 5) { err("Bad pin mode"); return; }
        gpio_set_mode(pin, GPIO_ALT, f);
    } else {
        err("Bad pin mode");
    }
}

// PIN pin, level     (statement form; the read form PIN(n) is in eval_function)
static void stmt_pin(void) {
    lex_next();                          // consume PIN
    if (gpio_guard()) return;
    int pin = gpio_pin_arg(); if (g_err) return;
    if (!expect(T_COMMA)) return;
    int v = (int)need_num(); if (g_err) return;
    gpio_write(pin, v != 0);
}

// PINSET mask / PINCLR mask : atomically set/clear every pin whose bit is 1.
static void stmt_pinset(int setit) {
    lex_next();                          // consume PINSET/PINCLR
    if (gpio_guard()) return;
    uint32_t mask = (uint32_t)(long)need_num(); if (g_err) return;
    mask &= 0x0FFFFFFFu;
    if (setit) gpio_set_mask(mask); else gpio_clr_mask(mask);
}

// I2C needs a real Pi (QEMU does not model the BSC); raise a clear error otherwise.
static int i2c_guard(void) {
    if (!i2c_available()) { err("I2C needs real Pi hardware"); return 1; }
    return 0;
}

// I2CWRITE addr, b1 [, b2, ...] : send the listed bytes to the device at `addr`.
static void stmt_i2cwrite(void) {
    lex_next();                          // consume I2CWRITE
    if (i2c_guard()) return;
    int addr = (int)need_num(); if (!expect(T_COMMA)) return;
    unsigned char buf[64]; int n = 0;
    for (;;) {
        int v = (int)need_num(); if (g_err) return;
        if (n >= (int)sizeof buf) { err("Too many bytes"); return; }
        buf[n++] = (unsigned char)v;
        if (tok != T_COMMA) break;
        lex_next();
    }
    if (i2c_write(addr, buf, n) < 0) err("I2C write failed");
}

// I2CREAD addr, buf, count : read `count` bytes from `addr` into the DIM buffer.
static void stmt_i2cread(void) {
    lex_next();                          // consume I2CREAD
    if (i2c_guard()) return;
    int addr = (int)need_num(); if (!expect(T_COMMA)) return;
    long ad = (long)need_num(); if (!expect(T_COMMA)) return;
    int n = (int)need_num(); if (g_err) return;
    if (n <= 0 || n > 65535) { err("Invalid argument"); return; }
    // The driver writes the buffer one byte at a time, so an unaligned DIM
    // address is safe (-mstrict-align); read straight into it.
    if (i2c_read(addr, (unsigned char *)(uintptr_t)ad, n) < 0) err("I2C read failed");
}

static void stmt_sound(void) {
    lex_next();                                  // consume SOUND
    if (tok == T_VAR && s_eq(tok_var, "OFF")) { lex_next(); sound_reset(); return; }
    int chan = (int)need_num();   if (!expect(T_COMMA)) return;
    int amp  = (int)need_num();   if (!expect(T_COMMA)) return;
    int pit  = (int)need_num();   if (!expect(T_COMMA)) return;
    int dur  = (int)need_num();   if (g_err) return;
    int vol = amp < 0 ? -amp : amp;              // accept BBC's -15..0 and a plain 0..15
    if (vol > 15) vol = 15;
    long dur_us = dur < 0 ? -1L : (long)dur * 50000L;    // a "twentieth" = 50 ms
    sound_enqueue(chan, pitch_to_hz(pit), vol, dur_us);
}

// TONE frequency_hz, duration_ms [, volume]   (direct, non-BBC helper)
// Plays on channel 0. Volume defaults to full (15).
static void stmt_tone(void) {
    lex_next();                                  // consume TONE
    int freq = (int)need_num();   if (!expect(T_COMMA)) return;
    int ms   = (int)need_num();
    int vol  = 15;
    if (tok == T_COMMA) { lex_next(); vol = (int)need_num(); if (vol > 15) vol = 15; }
    if (g_err) return;
    long dur_us = ms < 0 ? -1L : (long)ms * 1000L;
    sound_enqueue(0, freq, vol, dur_us);
}

// SCREEN width, height : switch the display resolution for this program.
// SCREEN (no arguments) restores the startup resolution. The system also restores
// it automatically when the program finishes (see run_program). The BBC logical
// coordinate system (0..1279 x 0..1023) is unchanged, so graphics keep working;
// only pixel density and the text grid change. Read the current size with the
// SCREENW / SCREENH functions.
