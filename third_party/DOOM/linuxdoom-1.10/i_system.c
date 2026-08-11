// i_system.c - BerryBasiC port of the DOOM system layer: timing, zone memory,
// clean exit and error reporting, over the BerryServices a POD is given.
// (Original id X11/Unix version replaced for the BerryBasiC platform.)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "doomdef.h"
#include "doomstat.h"
#include "m_misc.h"
#include "i_video.h"
#include "i_sound.h"
#include "d_net.h"
#include "g_game.h"
#include "i_system.h"

#include <berry_services.h>      // berry_svc: time_cs(), the machine clock

// Megabytes of zone memory to request. DOOM caches WAD lumps in the zone, so a
// few MB is plenty for the shareware game; the POD heap (48 MB) covers it.
int  mb_used = 8;

void I_Tactile (int on, int off, int total)
{
    (void)on; (void)off; (void)total;   // no force feedback
}

static ticcmd_t emptycmd;
ticcmd_t* I_BaseTiccmd (void)
{
    return &emptycmd;
}

// Zone memory: hand DOOM one big malloc'd block to sub-allocate.
byte* I_ZoneBase (int* size)
{
    *size = mb_used * 1024 * 1024;
    return (byte*) malloc (*size);
}

// Current time in 35 Hz tics, from the machine's centisecond clock.
int I_GetTime (void)
{
    unsigned cs = berry_svc->time_cs();
    return (int)((unsigned long long)cs * TICRATE / 100);
}

void I_Init (void)
{
    I_InitSound();      // silent stubs on this platform
}

void I_StartFrame (void)
{
    // Nothing asynchronous to service (no joystick).
}

// Poll input once per tic and turn it into DOOM events (implemented in i_video).
void I_StartTic (void)
{
    I_GetEvent();
}

byte* I_AllocLow (int length)
{
    byte* mem = (byte*) malloc (length);
    memset (mem, 0, length);
    return mem;
}

void I_Quit (void)
{
    D_QuitNetGame ();
    I_ShutdownSound();
    I_ShutdownGraphics();
    exit (0);
}

void I_Error (char* error, ...)
{
    va_list argptr;
    va_start (argptr, error);
    fprintf (stderr, "Error: ");
    vfprintf (stderr, error, argptr);
    fprintf (stderr, "\n");
    va_end (argptr);

    fflush (stderr);
    I_ShutdownGraphics ();
    exit (-1);
}
