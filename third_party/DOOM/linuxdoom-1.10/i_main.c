// i_main.c - BerryBasiC port: the POD entry point. It declares the POD's name,
// description and required capabilities, stashes the command line for DOOM's
// argument parser, and hands control to the game. D_DoomMain never returns (it
// enters the game loop). (Original id entry replaced for the BerryBasiC platform.)

#include <pod.h>

#include "doomdef.h"
#include "m_argv.h"
#include "d_main.h"

POD_NAME("doom")
POD_DESCRIPTION("DOOM (id Software) - needs DOOM1.WAD on the card")
POD_NEEDS(CAP_CONSOLE | CAP_GRAPHICS | CAP_FILES | CAP_HEAP,
          "CONSOLE=keyboard input; GRAPHICS=the framebuffer; "
          "FILES=reads the WAD; HEAP=zone memory")

int main (int argc, char** argv)
{
    myargc = argc;
    myargv = argv;

    D_DoomMain ();          // does not return
    return 0;
}
