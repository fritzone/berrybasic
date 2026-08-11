// i_video.c - BerryBasiC port of the DOOM video + input layer. It composes each
// 320x200 8-bit frame that the renderer draws into screens[0], then pushes it to
// the machine framebuffer in ONE call (the blitindexed service), integer-scaled
// and centred. Input is polled from the BerryBasiC keyboard and turned into DOOM
// events. (Original id X11 version replaced for the BerryBasiC platform.)

#include <string.h>

#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "d_main.h"
#include "doomdef.h"
#include "i_video.h"

#include <graphics.h>            // initgraph/blitindexed/flippage/setdoublebuffer
#include <berry_services.h>      // berry_svc: inkey(), keymods()

// BerryBasiC modifier bitmask values (mirror drivers/usb_hid.h).
#define KMOD_SHIFT 0x001
#define KMOD_CTRL  0x002
#define KMOD_ALT   0x004

// --- display geometry -------------------------------------------------------
static unsigned int pal32[256];     // current palette as 0xRRGGBB
static int scr_w, scr_h;            // machine framebuffer size (pixels)
static int scale = 1;               // integer upscale of the 320x200 image
static int ox, oy;                  // top-left of the blitted image (centred)

void I_InitGraphics (void)
{
    if (!initgraph()) return;               // no framebuffer (host build)
    scr_w = getmaxx() + 1;
    scr_h = getmaxy() + 1;
    scale = scr_w / SCREENWIDTH;
    if (scr_h / SCREENHEIGHT < scale) scale = scr_h / SCREENHEIGHT;
    if (scale < 1) scale = 1;
    ox = (scr_w - SCREENWIDTH  * scale) / 2;
    oy = (scr_h - SCREENHEIGHT * scale) / 2;
    setdoublebuffer(1);                     // compose each frame off-screen
    cleardevice();
}

void I_ShutdownGraphics (void)
{
    setdoublebuffer(0);
}

// Store the 256-colour palette (768 bytes RGB), gamma-corrected like the original.
void I_SetPalette (byte* palette)
{
    for (int i = 0; i < 256; i++)
    {
        int r = gammatable[usegamma][*palette++];
        int g = gammatable[usegamma][*palette++];
        int b = gammatable[usegamma][*palette++];
        pal32[i] = ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
    }
}

void I_UpdateNoBlit (void) { }

// Push the finished frame to the screen: one paletted blit, then show it.
void I_FinishUpdate (void)
{
    blitindexed(screens[0], pal32, SCREENWIDTH, SCREENHEIGHT, ox, oy, scale);
    flippage();
}

void I_ReadScreen (byte* scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

// Pause `count` vertical retraces (1/70 s each), so the screen wipe is paced.
void I_WaitVBL (int count)
{
    unsigned t0 = berry_svc->time_cs();
    unsigned d  = (unsigned)count * 100u / 70u;     // 1/70 s -> centiseconds
    if (!d) d = 1;
    while (berry_svc->time_cs() - t0 < d) { }
}

void I_BeginRead (void) { }
void I_EndRead (void)   { }

// --- input ------------------------------------------------------------------
// Translate a BerryBasiC key code into a DOOM key code.
static int xlate (int k)
{
    switch (k)
    {
        case 0x11: return KEY_LEFTARROW;
        case 0x12: return KEY_RIGHTARROW;
        case 0x13: return KEY_UPARROW;
        case 0x14: return KEY_DOWNARROW;
        case 0x1B: return KEY_ESCAPE;
        case 13:                       // CR (serial) or LF (USB HID) -> Enter
        case 10:   return KEY_ENTER;
        case 9:    return KEY_TAB;
        case 8:
        case 0x7F: return KEY_BACKSPACE;
    }
    if (k >= 0x101 && k <= 0x10C) return KEY_F1 + (k - 0x101);   // F1..F12
    if (k >= 'A' && k <= 'Z') return k + 32;                     // DOOM wants lowercase
    if (k >= 32 && k <= 126)  return k;                          // printable ASCII
    return 0;
}

static void post (evtype_t type, int key)
{
    event_t ev;
    ev.type = type; ev.data1 = key; ev.data2 = ev.data3 = 0;
    D_PostEvent(&ev);
}

// Post a modifier as a held key: keydown when it goes down, keyup when it lifts.
// This gives fire (Ctrl), run (Shift) and strafe (Alt) proper hold behaviour
// even though the character keyboard reports only presses.
static void modkey (int now, int was, int mask, int doomkey)
{
    if ((now & mask) && !(was & mask)) post(ev_keydown, doomkey);
    else if (!(now & mask) && (was & mask)) post(ev_keyup, doomkey);
}

// Poll the keyboard once and turn it into DOOM events. A USB keyboard reports
// which keys are currently HELD, so we diff against the previous poll to make
// real keydown/keyup pairs - the hold-to-move the game needs. A serial terminal
// has no held state, so each character is posted as a down+up tap (fine for
// menus). Ctrl/Shift/Alt are held modifiers either way (fire / run / strafe).
void I_GetEvent (void)
{
    // Drain the key FIFO: this polls the keyboard (refreshing the held-key
    // report) and, on serial, gives us the typed characters.
    int chars[16], nchars = 0, k;
    for (int g = 0; g < 32 && (k = berry_svc->inkey(0)) >= 0; g++)
        if (nchars < 16) chars[nchars++] = k;

    int cur[8];
    int ncur = berry_svc->keys_down ? berry_svc->keys_down(cur, 8) : -1;  // <0 = serial

    // DOOM keycodes we have an outstanding keydown for. A key is kept "down"
    // while it is in the held set (cur); a typed key that isn't held (a quick tap,
    // or serial where there is no held state) is released the same poll.
    static int down[16];
    static int ndown = 0;

    // 1. Newly-typed keys -> keydown (catches quick taps, so menus respond).
    for (int i = 0; i < nchars; i++)
    {
        int dk = xlate(chars[i]);
        if (!dk) continue;
        int have = 0;
        for (int j = 0; j < ndown; j++) if (down[j] == dk) { have = 1; break; }
        if (!have && ndown < 16) { post(ev_keydown, dk); down[ndown++] = dk; }
    }
    // 2. Held keys we haven't marked down yet -> keydown (if inkey missed the press).
    for (int i = 0; i < ncur; i++)
    {
        int dk = xlate(cur[i]);
        if (!dk) continue;
        int have = 0;
        for (int j = 0; j < ndown; j++) if (down[j] == dk) { have = 1; break; }
        if (!have && ndown < 16) { post(ev_keydown, dk); down[ndown++] = dk; }
    }
    // 3. Release any tracked key that is no longer held (all of them on serial).
    for (int i = 0; i < ndown; )
    {
        int held = 0;
        for (int j = 0; j < ncur; j++) if (xlate(cur[j]) == down[i]) { held = 1; break; }
        if (!held) { post(ev_keyup, down[i]); down[i] = down[--ndown]; }
        else i++;
    }

    // Ctrl/Shift/Alt are held state from the modifier byte, tracked separately.
    static int prev_mods = 0;
    int mods = berry_svc->keymods();
    modkey(mods, prev_mods, KMOD_CTRL,  KEY_RCTRL);
    modkey(mods, prev_mods, KMOD_SHIFT, KEY_RSHIFT);
    modkey(mods, prev_mods, KMOD_ALT,   KEY_RALT);
    prev_mods = mods;
}
