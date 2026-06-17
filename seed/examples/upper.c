// upper.sed - string in, string out: upper-case the first argument.
// Demonstrates the snapshot-into-scratch string argument and the string return
// path (svc->set_return_str, read back with CALL$).
//
//   SEED h%, "UPPER.SED"
//   PRINT CALL$(h%, "berry")     -> BERRY
#include "seed.h"

SEED_EXPORT(seed)
{
    if (argc < 1 || !argv[0].is_str) return 0;

    char buf[256];
    int n = argv[0].len;
    if (n > (int)sizeof(buf)) n = sizeof(buf);
    for (int i = 0; i < n; i++) {
        char c = argv[0].str[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        buf[i] = c;
    }
    svc->set_return_str(buf, n);
    return n;                       // numeric return = length, for convenience
}
