// dynarr.sed - build 1..n in a realloc-grown buffer and return the sum.
// Demonstrates the full C allocation API: realloc (grow a buffer one element at
// a time) and aligned_alloc (a 64-byte cache-aligned scratch copy).
//
//   SEED h%, "DYNARR.SED"
//   PRINT CALL(h%, 100)          -> 5050
#include "seed.h"
#include <stdlib.h>     // malloc / realloc / aligned_alloc / free

SEED_EXPORT(dynarr)
{
    int n = (argc > 0) ? (int)argv[0].num : 0;
    if (n <= 0) return 0;

    // Classic grow-as-you-go pattern with realloc.
    int *a = 0;
    for (int i = 0; i < n; i++) {
        int *na = realloc(a, (i + 1) * sizeof(int));
        if (!na) { free(a); return -1; }
        a = na;
        a[i] = i + 1;
    }

    // Copy into a 64-byte (cache-line) aligned buffer, just to exercise it.
    int *aligned = aligned_alloc(64, n * sizeof(int));
    if (!aligned) { free(a); return -1; }
    for (int i = 0; i < n; i++) aligned[i] = a[i];

    long sum = 0;
    for (int i = 0; i < n; i++) sum += aligned[i];

    free(aligned);
    free(a);
    return (double)sum;                 // 1+2+...+n = n(n+1)/2
}
