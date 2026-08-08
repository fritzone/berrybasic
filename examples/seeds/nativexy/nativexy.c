// NATIVEXY.SED - draws a crosshair at a coordinate BASIC hands it, in whatever
// graphics mode BASIC is using, and returns that mode.
//
//   NATIVEXY x, y            ' a keyword statement: crosshair at BASIC's (x,y)
//   PRINT GMODE              ' ... and it matches what the seed returned
//
// The point of the demo: drawing through the BGI/gfx_* calls is ALWAYS device
// pixels (top-left, y down), but the (x,y) BASIC passed are in BASIC's *current*
// mode. gfx_from_basic() converts them so the crosshair lands exactly where
// BASIC's own `PLOT 69, x, y` would put it. In MODE 2 that conversion is an
// identity - which is the whole reason MODE 2 makes mixed BASIC+seed graphics
// simple: the two coordinate systems finally coincide.
#include "seed.h"
#include <graphics.h>

SEED_KEYWORD("NATIVEXY", SEED_KW_STATEMENT, 2, 2) {
    (void)argc;
    if (!initgraph()) return 0;                  // no framebuffer (host build)

    int px, py;
    gfx_from_basic((int)argv[0].num, (int)argv[1].num, &px, &py);

    setrgbcolor(255, 255, 0);
    line(px - 12, py, px + 12, py);              // device pixels, top-left origin
    line(px, py - 12, px, py + 12);

    return gfx_mode();                           // 1 or 2 (statement result is ignored,
}                                                //   but handy when called via CALL)
