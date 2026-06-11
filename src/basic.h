#ifndef BASIC_H
#define BASIC_H

// Portable BASIC interpreter. Depends only on console.h, so it builds unchanged
// for both the bare-metal target and the host test harness.

void basic_init(void);   // reset program, variables, and state
void basic_repl(void);   // read-eval-print loop (returns on host EOF)

#endif
