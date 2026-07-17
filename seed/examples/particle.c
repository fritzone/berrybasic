// PARTICLE.SED - the record payoff case: step a whole array of TYPE records at
// native speed through a direct, zero-copy pointer into their storage.
//
// Adds a statement keyword to BASIC:
//
//   TYPE particle : x, y, vx, vy : ENDTYPE
//   DIM p(999) AS particle
//   ... fill p() ...
//   PARTICLE "P", 0.1            ' advance every particle by dt, bounce at the edges
//
// A record is reached by *name*, exactly like a numeric array - it is never
// passed in as an argument. The numeric fields of one element are contiguous,
// so the whole array is a strided block of doubles: element e's field f is
// base[e * stride + f]. That is what makes a thousand particles worth doing
// natively instead of in the interpreter.
#include "seed.h"

#define W 1280.0
#define H 1024.0

SEED_KEYWORD("PARTICLE", SEED_KW_STATEMENT, 2, 2) {
    (void)argc;
    if (!argv[0].is_str) return 0;

    char name[16];
    int n = argv[0].len;
    if (n > (int)sizeof(name) - 1) n = sizeof(name) - 1;
    for (int i = 0; i < n; i++) name[i] = argv[0].str[i];
    name[n] = 0;

    int nelem = 0, stride = 0;
    double *p = svc->rec_array(name, &nelem, &stride);
    if (!p) return 0;                       // not a record: nothing to do

    // Look the fields up once, by name, then index with plain arithmetic.
    int fx  = svc->rec_field(name, "X"),  fy  = svc->rec_field(name, "Y");
    int fvx = svc->rec_field(name, "VX"), fvy = svc->rec_field(name, "VY");
    if (fx < 0 || fy < 0 || fvx < 0 || fvy < 0) return 0;   // wrong shape of record

    double dt = argv[1].num;
    for (int e = 0; e < nelem; e++) {
        double *r = &p[e * stride];         // this element's numeric fields
        r[fx] += r[fvx] * dt;
        r[fy] += r[fvy] * dt;
        if (r[fx] < 0.0) { r[fx] = 0.0; r[fvx] = -r[fvx]; }
        if (r[fx] > W)   { r[fx] = W;   r[fvx] = -r[fvx]; }
        if (r[fy] < 0.0) { r[fy] = 0.0; r[fvy] = -r[fvy]; }
        if (r[fy] > H)   { r[fy] = H;   r[fvy] = -r[fvy]; }
    }
    return 0;
}
