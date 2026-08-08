// add.sed - the simplest possible seed: number in, number out.
// Proves the toolchain / cache / position-independence pipeline end to end.
//
//   SEED h%, "ADD.SED"
//   PRINT CALL(h%, 40, 2)        -> 42
#include "seed.h"

SEED_EXPORT(seed)
{
    (void)svc;
    double a = (argc > 0) ? argv[0].num : 0;
    double b = (argc > 1) ? argv[1].num : 0;
    return a + b;
}
