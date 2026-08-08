// PAINT.SED - keyboard and mouse from inside a seed.
//
// Adds a statement keyword to BASIC:
//
//   PAINT                    ' drag with the left button to draw; Q or ESC quits
//
// Two things worth knowing, both of which this loop shows:
//
//  * The mouse call IS the poll. The interpreter services the pointer between
//    BASIC statements, so while a seed runs nothing else is moving it. A seed
//    that wants a live mouse polls it itself, in its own loop - which is why an
//    interactive seed like this one has to be written as a loop rather than as
//    a thing called once per frame from BASIC.
//
//  * svc->inkey(0) is the non-blocking read: it returns -1 straight away when
//    no key is waiting, so the loop keeps drawing. svc->getkey() would block
//    and the pointer would freeze. Both read the same key stream as BASIC's
//    GET / INKEY.
//
// Mouse coordinates are raw framebuffer pixels, which is the same space the
// gfx_* calls draw in, so a position can be used as a coordinate directly.
#include "seed.h"
#include <graphics.h>

#define BTN_LEFT   1
#define BTN_RIGHT  2

SEED_KEYWORD("PAINT", SEED_KW_STATEMENT, 0, 0) {
    (void)argv; (void)argc;
    if (!svc->gfx_avail()) return 0;             // no framebuffer (host build)

    int px = -1, py = -1;                        // previous point, -1 = pen up
    int colour = 15;

    for (;;) {
        int k = svc->inkey(0);                   // non-blocking: -1 if no key
        if (k == 'q' || k == 'Q' || k == 27) break;
        if (k >= '1' && k <= '7') colour = k - '0';

        int x, y, b;
        svc->mouse(&x, &y, &b);                  // this call polls the hardware

        if (b & BTN_RIGHT) {                     // right button clears
            svc->gfx_clear(0);
            px = py = -1;
        } else if (b & BTN_LEFT) {
            if (px >= 0) svc->gfx_line(px, py, x, y, colour);
            else         svc->gfx_putpixel(x, y, colour);
            px = x; py = y;                      // drag: join the samples up
        } else {
            px = py = -1;                        // button up: lift the pen
        }
    }
    return 0;
}
