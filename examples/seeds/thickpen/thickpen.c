// THICKPEN.SED - shows off the seed line pen: width, and the three join styles.
//
//   THICKPEN                 ' draws three thick zig-zags, one per join style
//
// Each row is the same open polyline stroked at width 14, with a different join
// (miter / bevel / round) - so the corners are sharp, clipped, and rounded in
// turn. The last row is a closed rectangle to show joins all the way round.
#include "seed.h"
#include <graphics.h>

static void zigzag(int y, int join, int r, int g, int b) {
    int pts[10] = { 80, y, 200, y + 90, 320, y, 440, y + 90, 560, y };
    setrgbcolor(r, g, b);
    setlinejoin(join);
    drawpoly(5, pts);                            // open polyline: joins at the 3 interior corners
}

SEED_KEYWORD("THICKPEN", SEED_KW_STATEMENT, 0, 0) {
    (void)argv; (void)argc;
    if (!initgraph()) return 0;                  // no framebuffer (host build)

    setlinewidth(14);
    setlinecap(CAP_ROUND);                       // round the open ends of the zig-zags
    zigzag(60,  JOIN_MITER, 255, 80,  80);
    zigzag(200, JOIN_BEVEL, 80,  255, 80);
    zigzag(340, JOIN_ROUND, 120, 160, 255);

    setlinejoin(JOIN_MITER);
    setrgbcolor(255, 230, 60);
    rectangle(700, 120, 1000, 420);              // closed: sharp mitred corners

    return gfx_mode();
}
