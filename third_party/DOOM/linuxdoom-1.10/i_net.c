// i_net.c - BerryBasiC port: networking is out of scope, so this is a
// single-player stub. It brings up a one-node "network" so the game logic runs
// standalone; there is no packet I/O. (Original id X11/BSD-sockets version
// replaced for the BerryBasiC platform.)

#include <stdlib.h>
#include <string.h>

#include "doomdef.h"
#include "i_system.h"
#include "d_event.h"
#include "d_net.h"
#include "m_argv.h"
#include "i_net.h"

doomcom_t*  doomcom;

// Set up a standalone single-player "network": one node, one player, no
// deathmatch. Mirrors the standalone path of the original I_InitNetwork.
void I_InitNetwork (void)
{
    doomcom = malloc (sizeof (*doomcom));
    memset (doomcom, 0, sizeof (*doomcom));

    doomcom->id            = DOOMCOM_ID;
    doomcom->numplayers    = doomcom->numnodes = 1;
    doomcom->deathmatch    = false;
    doomcom->consoleplayer = 0;
    doomcom->ticdup        = 1;
    doomcom->extratics     = 0;
}

void I_NetCmd (void)
{
    // No networking: nothing to send or receive.
}
