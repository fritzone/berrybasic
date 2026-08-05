/* A minimal BerryBasiC POD program.  Build it with:
 *
 *     tcc -pod pod_hello.c -o HELLO.POD
 *
 * and inspect the result with tests/podcheck.py.  It declares only the CONSOLE
 * capability, so the loader hands it a services table in which everything else
 * is a refusal stub. */
#include <pod.h>

POD_NAME("hello")
POD_VERSION("1.0")
POD_AUTHOR("fritzone")
POD_DESCRIPTION("smallest useful POD: greets the console and exits")
POD_NEEDS(CAP_CONSOLE, "CONSOLE=prints a greeting")

int pod_main(const BerryServices *svc, int argc, const char *const *argv)
{
    static const char msg[] = "hello from a pod\n";
    svc->puts(msg, sizeof msg - 1);
    return 0;
}
