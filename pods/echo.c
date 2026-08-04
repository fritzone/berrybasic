/* echo - print the arguments, space-separated. A BerryBasiC /sys command POD.
 *
 * Demonstrates the command shell: typed at the prompt as `echo hello world`,
 * the interpreter finds /sys/ECHO.POD and runs it with argv = {"echo","hello",
 * "world"}. It declares only CONSOLE, so its services table can do nothing else.
 */
#include <pod.h>

POD_NAME("echo")
POD_DESCRIPTION("print the arguments")
POD_NEEDS(CAP_CONSOLE, "CONSOLE=writes its arguments to the screen")

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

int pod_main(const PodServices *svc, int argc, const char *const *argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) svc->putc(' ');
        svc->puts(argv[i], slen(argv[i]));
    }
    svc->putc('\n');
    return 0;
}
