// bufsum.sed - sum the bytes of a buffer that BASIC built with DIM + ?.
// Shows the memory bridge: BASIC reserves a block and pokes bytes into it, then
// passes the raw address to a seed that processes it at native speed.
//
//   DIM B% 9
//   FOR I = 0 TO 9 : B%?I = I*I : NEXT
//   SEED h%, "BUFSUM.SED"
//   PRINT CALL(h%, B%, 10)        -> sum of the 10 bytes
#include "seed.h"

SEED_EXPORT(bufsum)
{
    if (argc < 2) return 0;
    const unsigned char *p = (const unsigned char *)(uintptr_t)(long)argv[0].num;
    int len = (int)argv[1].num;
    long sum = 0;
    for (int i = 0; i < len; i++) sum += p[i];
    return (double)sum;
}
