// median.sed - the median of a numeric array, passed by name.
// Needs scratch memory (a sorted copy of the data), so it demonstrates malloc /
// free - which read like ordinary C but draw from the seed heap.
//
//   DIM A(5) : ... fill A() ...
//   SEED h%, "MEDIAN.SED"
//   PRINT CALL(h%, "A")
#include "seed.h"
#include <stdlib.h>     // malloc / free

SEED_EXPORT(seed)
{
    if (argc < 1 || !argv[0].is_str) return 0;

    char name[16];
    int n = argv[0].len;
    if (n > 15) n = 15;
    for (int i = 0; i < n; i++) name[i] = argv[0].str[i];
    name[n] = 0;

    int len = 0;
    double *a = svc->num_array(name, &len);
    if (!a || len <= 0) return 0;

    double *tmp = malloc(len * sizeof(double));   // heap copy to sort
    if (!tmp) return 0;                           // out of heap
    for (int i = 0; i < len; i++) tmp[i] = a[i];

    for (int i = 1; i < len; i++) {            // insertion sort (leaves a[] untouched)
        double v = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = v;
    }

    double med = tmp[len / 2];
    free(tmp);
    return med;
}
