// i_sound.c - BerryBasiC port: real sound effects. DOOM's sfx are 8-bit unsigned
// PCM lumps (DS<name>); this mixes up to 8 active channels into a stereo stream
// and hands it to the machine's streamed-PCM audio service (DMA-fed PWM on the
// analogue jack). Music is still stubbed (MUS synthesis is a separate job).
// Authored for the BerryBasiC platform (replaces id's sound-server version).

#include <stdio.h>
#include <string.h>

#include "doomdef.h"
#include "doomstat.h"
#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"
#include "w_wad.h"
#include "z_zone.h"
#include "sounds.h"

#include <berry_services.h>      // berry_svc->audio_*

// Referenced (extern) by the sound module; unused with no external sound server.
FILE*  sndserver = 0;
char*  sndserver_filename = 0;

#define SND_RATE     11025       // output sample rate (DOOM sfx are ~11025 Hz)
#define NUM_CHANNELS 8
#define MIX_CHUNK    1024        // max stereo frames mixed per I_UpdateSound call

typedef struct {
    int                  active;
    const unsigned char *data;   // sample bytes (past the 8-byte DMX header)
    unsigned int         len;     // number of samples
    unsigned int         pos;     // 16.16 fixed-point read position
    unsigned int         base;    // 16.16 step at pitch 128 (accounts for sfx rate)
    unsigned int         step;    // 16.16 advance per output sample (base * pitch/128)
    int                  leftvol; // 0..127
    int                  rightvol;
    int                  handle;
} chan_t;

static chan_t chans[NUM_CHANNELS];
static int    g_handle;
static short  g_mix[MIX_CHUNK * 2];

void I_SetChannels (void) { }

// "ds" + up-to-6-char sfx name -> lump number; also caches the lump so the game
// code's "not pre-cached" path never fires. Returns -1 if the lump is absent
// (e.g. a full-game monster sound missing from the shareware WAD - it just won't play).
int I_GetSfxLumpNum (sfxinfo_t* sfx)
{
    char name[16];
    name[0] = 'd'; name[1] = 's';
    int i = 0;
    while (sfx->name[i] && i < 6) { name[2 + i] = sfx->name[i]; i++; }
    name[2 + i] = 0;

    int lump = W_CheckNumForName (name);
    if (lump >= 0 && !sfx->data)
        sfx->data = W_CacheLumpNum (lump, PU_STATIC);
    return lump;
}

void I_InitSound (void)
{
    for (int i = 0; i < NUM_CHANNELS; i++) chans[i].active = 0;
    berry_svc->audio_open (SND_RATE);
}

void I_ShutdownSound (void) { berry_svc->audio_close (); }

// volume 0..127; sep 0 = hard left, 128 = centre, 255 = hard right.
static void set_vol (chan_t* c, int vol, int sep)
{
    if (vol < 0) vol = 0; else if (vol > 127) vol = 127;
    if (sep < 0) sep = 0; else if (sep > 255) sep = 255;
    c->leftvol  = (sep <= 128) ? vol : vol * (255 - sep) / 127;
    c->rightvol = (sep >= 128) ? vol : vol * sep / 127;
}

int I_StartSound (int id, int vol, int sep, int pitch, int priority)
{
    (void)priority;
    if (id <= 0 || id >= NUMSFX) return -1;
    sfxinfo_t* sfx = &S_sfx[id];
    if (sfx->link) sfx = sfx->link;

    if (sfx->lumpnum < 0) sfx->lumpnum = I_GetSfxLumpNum (sfx);
    if (sfx->lumpnum < 0) return -1;
    if (!sfx->data) sfx->data = W_CacheLumpNum (sfx->lumpnum, PU_STATIC);

    unsigned int lumplen = W_LumpLength (sfx->lumpnum);
    if (lumplen <= 8) return -1;
    const unsigned char* lump = (const unsigned char*) sfx->data;
    unsigned int rate = lump[2] | (lump[3] << 8);
    if (rate < 4000 || rate > 48000) rate = SND_RATE;
    unsigned int hdrlen = lump[4] | (lump[5] << 8) | (lump[6] << 16) | ((unsigned)lump[7] << 24);
    unsigned int avail  = lumplen - 8;
    unsigned int len    = (hdrlen && hdrlen < avail) ? hdrlen : avail;

    // Pick a channel: a free one, else steal round-robin.
    int ch = -1;
    for (int i = 0; i < NUM_CHANNELS; i++) if (!chans[i].active) { ch = i; break; }
    if (ch < 0) { static int rr = 0; ch = rr; rr = (rr + 1) % NUM_CHANNELS; }

    chan_t* c = &chans[ch];
    c->data = lump + 8;
    c->len  = len;
    c->pos  = 0;
    c->base = (unsigned int)(((unsigned long long)(1u << 16) * rate) / SND_RATE);
    c->step = (unsigned int)(((unsigned long long)c->base * (pitch <= 0 ? 128 : pitch)) / 128);
    set_vol (c, vol, sep);
    c->handle = ++g_handle;
    if (c->handle <= 0) c->handle = g_handle = 1;
    c->active = 1;
    return c->handle;
}

void I_StopSound (int handle)
{
    for (int i = 0; i < NUM_CHANNELS; i++)
        if (chans[i].active && chans[i].handle == handle) chans[i].active = 0;
}

int I_SoundIsPlaying (int handle)
{
    for (int i = 0; i < NUM_CHANNELS; i++)
        if (chans[i].active && chans[i].handle == handle) return 1;
    return 0;
}

void I_UpdateSoundParams (int handle, int vol, int sep, int pitch)
{
    for (int i = 0; i < NUM_CHANNELS; i++)
        if (chans[i].active && chans[i].handle == handle) {
            set_vol (&chans[i], vol, sep);
            chans[i].step = (unsigned int)(((unsigned long long)chans[i].base *
                                            (pitch <= 0 ? 128 : pitch)) / 128);
        }
}

// Mix as many frames as the audio ring has room for, and feed them in. Driven
// each frame from D_Display (I_UpdateSound then I_SubmitSound).
void I_UpdateSound (void)
{
    int avail = berry_svc->audio_avail ();
    while (avail > 0) {
        int n = avail > MIX_CHUNK ? MIX_CHUNK : avail;
        for (int f = 0; f < n; f++) {
            int l = 0, r = 0;
            for (int i = 0; i < NUM_CHANNELS; i++) {
                chan_t* c = &chans[i];
                if (!c->active) continue;
                unsigned int idx = c->pos >> 16;
                if (idx >= c->len) { c->active = 0; continue; }
                int s = (int)c->data[idx] - 128;      // unsigned 8-bit -> signed
                l += s * c->leftvol;
                r += s * c->rightvol;
                c->pos += c->step;
            }
            if (l >  32767) l =  32767; else if (l < -32768) l = -32768;
            if (r >  32767) r =  32767; else if (r < -32768) r = -32768;
            g_mix[f * 2]     = (short)l;
            g_mix[f * 2 + 1] = (short)r;
        }
        int wrote = berry_svc->audio_write (g_mix, n);
        avail -= wrote;
        if (wrote < n) break;                          // ring full
    }
}

void I_SubmitSound (void) { }

// --- music: still stubbed (MUS -> synth is a separate project) --------------
void I_InitMusic (void)              { }
void I_ShutdownMusic (void)          { }
void I_SetMusicVolume (int volume)   { (void)volume; }
void I_PauseSong (int handle)        { (void)handle; }
void I_ResumeSong (int handle)       { (void)handle; }
int  I_RegisterSong (void* data)     { (void)data; return 0; }
void I_PlaySong (int handle, int looping) { (void)handle; (void)looping; }
void I_StopSong (int handle)         { (void)handle; }
void I_UnRegisterSong (int handle)   { (void)handle; }
