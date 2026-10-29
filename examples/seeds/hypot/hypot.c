// HYPOT.SED — adds a numeric-function keyword to BASIC:  r = HYPOT(dx, dy)
//
// Built with SEED_KEYWORD, so once this .SED is in /seed the interpreter picks
// it up at startup and HYPOT( ) works with no SEED/CALL.  Returns the length of
// the hypotenuse, sqrt(dx*dx + dy*dy).
#include "seed.h"

// The seed runtime has no <math.h> (and linking one in would break the
// relocation-free rule), so compute the square root locally with a few
// Newton-Raphson steps — plenty for a demo.
static double my_sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double g = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 40; i++) g = 0.5 * (g + x / g);
    return g;
}

SEED_KEYWORD("HYPOT", SEED_KW_NUMFN, 2, 2) {
    (void)svc; (void)argc;
    double dx = argv[0].num, dy = argv[1].num;
    return my_sqrt(dx * dx + dy * dy);
}
