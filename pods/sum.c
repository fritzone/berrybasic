/* sum - add up the integer arguments and print the total. A /sys command POD.
 *
 * `sum 40 2 100` prints 142. Shows a command parsing its own argv and doing
 * real work with no libc underneath it - just the CONSOLE services.
 */
#include <pod.h>

POD_NAME("sum")
POD_DESCRIPTION("add the integer arguments")
POD_NEEDS(CAP_CONSOLE, "CONSOLE=prints the total")

static long parse_long(const char *s) {
    long v = 0; int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static void put_long(const PodServices *svc, long v) {
    char tmp[24]; int m = 0, neg = 0;
    unsigned long x;
    if (v < 0) { neg = 1; x = (unsigned long)(-v); } else x = (unsigned long)v;
    if (x == 0) tmp[m++] = '0';
    while (x) { tmp[m++] = (char)('0' + (int)(x % 10)); x /= 10; }
    char out[26]; int n = 0;
    if (neg) out[n++] = '-';
    while (m) out[n++] = tmp[--m];
    svc->puts(out, n);
}

int pod_main(const PodServices *svc, int argc, const char *const *argv)
{
    long total = 0;
    for (int i = 1; i < argc; i++) total += parse_long(argv[i]);
    put_long(svc, total);
    svc->putc('\n');
    return 0;
}
