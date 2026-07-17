// SHOUT.SED — adds a statement keyword to BASIC:  SHOUT "hello"
//
// Built with SEED_KEYWORD (kind SEED_KW_STATEMENT), so it is used as a command,
// not a function. Prints its string argument upper-cased with an exclamation.
#include "seed.h"
#include <ctype.h>

SEED_KEYWORD("SHOUT", SEED_KW_STATEMENT, 1, 1) {
    (void)argc;
    const seed_arg *a = &argv[0];
    if (a->is_str)
        for (int i = 0; i < a->len; i++)
            svc->putc(toupper((unsigned char)a->str[i]));
    svc->putc('!');
    svc->putc('\n');
    return 0;
}
