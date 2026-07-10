// Host backend for sound.h. There is no portable, dependency-free way to make a
// Linux terminal beep, and the unit tests inspect the interpreter's own audible
// state (sound_cur_freq / sound_cur_vol), not the hardware. So the host voice is
// a silent stub that just remembers the last request, which is handy when
// debugging the native `basic_host` build under a debugger.

#include "sound.h"

int host_snd_freq = 0;    // last tone requested (0 = silent), for inspection
int host_snd_vol  = 0;

void snd_init(void) { host_snd_freq = 0; host_snd_vol = 0; }

void snd_set_tone(int freq_hz, int vol) {
    if (freq_hz <= 0 || vol <= 0) { host_snd_freq = 0; host_snd_vol = 0; return; }
    host_snd_freq = freq_hz;
    host_snd_vol  = vol;
}

void snd_silence(void) { host_snd_freq = 0; host_snd_vol = 0; }
