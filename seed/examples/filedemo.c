// filedemo.sed - write a small report to the SD card with fprintf, then read it
// back with fgets, showing the seed <stdio.h> file access (ABI v5).
//
//   SEED h%, "FILEDEMO.SED"
//   n% = CALL(h%, 5)     : REM write 5 rows to SEEDLOG.TXT, return the rows read back
//
// Returns the number of lines read back (so it equals the argument on success),
// or -1 if a file could not be opened.
#include "seed.h"
#include <stdio.h>

SEED_EXPORT(filedemo)
{
    int rows = argc >= 1 ? (int)argv[0].num : 3;

    FILE *f = fopen("SEEDLOG.TXT", "w");
    if (!f) return -1;
    double sum = 0;
    for (int i = 1; i <= rows; i++) {
        fprintf(f, "row %d: %d squared is %d\n", i, i, i * i);
        sum += (double)(i * i);
    }
    fprintf(f, "mean square = %f\n", sum / rows);   // %f -> BASIC number style
    fclose(f);

    f = fopen("SEEDLOG.TXT", "r");
    if (!f) return -1;
    int lines = 0;
    char buf[128];
    while (fgets(buf, sizeof buf, f)) lines++;
    fclose(f);
    return (double)lines;
}
