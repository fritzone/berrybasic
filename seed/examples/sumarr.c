// sumarr.sed - the payoff case: crunch a whole numeric array at native speed
// through a direct, zero-copy pointer into its storage.
//
//   DIM A(1000)
//   ... fill A() ...
//   SEED h%, "SUMARR.SED"
//   PRINT CALL(h%, "A")          -> sum of A(0..1000)
#include "seed.h"

SEED_EXPORT(seed)
{
    if (argc < 1 || !argv[0].is_str) return 0;

    char name[16];
    int n = argv[0].len;
    if (n > (int)sizeof(name) - 1) n = sizeof(name) - 1;
    for (int i = 0; i < n; i++) name[i] = argv[0].str[i];
    name[n] = 0;

    int len = 0;
    double *a = svc->num_array(name, &len);
    if (!a) return 0;

    double sum = 0;
    for (int i = 0; i < len; i++) sum += a[i];
    return sum;
}
