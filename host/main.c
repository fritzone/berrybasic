// Host test driver: run the BASIC interpreter against stdin/stdout.
//   make host && ./build/basic_host          (interactive)
//   echo '10 PRINT 6*7' | ./build/basic_host  (scripted)
#include "basic.h"

int main(void) {
    basic_init();
    basic_repl();
    return 0;
}
