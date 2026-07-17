// UPRECS.SED - the text half of record access from a seed.
//
// Adds a statement keyword to BASIC:
//
//   TYPE player : name$, score : ENDTYPE
//   DIM team(2) AS player
//   ... fill team() ...
//   UPRECS "TEAM", "NAME$"       ' uppercase that text field of every element
//
// Text fields are not in the zero-copy block that rec_array() hands out - the
// interpreter is free to move string bytes around when it compacts its heap -
// so they are copied out and back by name, one element at a time. rec_array is
// still how a seed learns how many elements there are.
#include "seed.h"

static void copy_arg(const seed_arg *a, char *out, int outsz) {
    int n = a->len;
    if (n > outsz - 1) n = outsz - 1;
    for (int i = 0; i < n; i++) out[i] = a->str[i];
    out[n] = 0;
}

SEED_KEYWORD("UPRECS", SEED_KW_STATEMENT, 2, 2) {
    (void)argc;
    if (!argv[0].is_str || !argv[1].is_str) return 0;

    char rec[16], field[16];
    copy_arg(&argv[0], rec, sizeof rec);
    copy_arg(&argv[1], field, sizeof field);

    int nelem = 0, stride = 0;
    if (!svc->rec_array(rec, &nelem, &stride)) return 0;   // not a record

    for (int e = 0; e < nelem; e++) {
        char buf[64];
        int n = svc->rec_get_str(rec, e, field, buf, sizeof buf);
        if (n <= 0) continue;                              // empty, or not a text field
        if (n > (int)sizeof buf) n = sizeof buf;           // get_str reports the full length
        for (int i = 0; i < n; i++)
            if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
        svc->rec_set_str(rec, e, field, buf, n);
    }
    return 0;
}
