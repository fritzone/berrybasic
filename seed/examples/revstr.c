// REVSTR.SED — adds a string-function keyword to BASIC:  a$ = REVERSE$("abc")
//
// Built with SEED_KEYWORD (kind SEED_KW_STRFN, name ends in '$'). Returns its
// argument reversed via the set_return_str service, which CALL$/expressions read.
#include "seed.h"

SEED_KEYWORD("REVERSE$", SEED_KW_STRFN, 1, 1) {
    (void)argc;
    const seed_arg *a = &argv[0];
    char buf[256];
    int n = a->len; if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    for (int i = 0; i < n; i++) buf[i] = a->str[n - 1 - i];
    svc->set_return_str(buf, n);
    return 0.0;
}
