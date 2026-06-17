// sortarr.sed - sort a numeric BASIC array ascending, in place, with qsort.
// Shows that standard <stdlib.h> (qsort) and <string.h> (memcpy) work in a seed.
//
//   DIM E(4) : E(0)=5 : E(1)=2 : ...
//   SEED h%, "SORTARR.SED"
//   CALL h%, "E"                 sorts E() in place; returns its length
#include "seed.h"
#include <stdlib.h>
#include <string.h>

static int cmp_asc(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);          // -1 / 0 / +1, no overflow
}

SEED_EXPORT(sortarr)
{
    if (argc < 1 || !argv[0].is_str) return 0;

    char name[16];
    int n = argv[0].len;
    if (n > 15) n = 15;
    memcpy(name, argv[0].str, n);
    name[n] = 0;

    int len = 0;
    double *a = svc->num_array(name, &len);    // direct pointer to the array
    if (!a || len <= 0) return 0;

    qsort(a, (size_t)len, sizeof(double), cmp_asc);
    return len;
}
