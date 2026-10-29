// Host backend for native seeds. Seeds are AArch64 machine code, so they cannot
// run inside the x86/host test build. The loader and header validation still work
// (handy for testing the SEED/CALL parsing), but actually invoking a seed reports
// "unsupported" so the host build degrades gracefully.
#include "seed.h"
#include "pod.h"

void icache_sync(const void *addr, unsigned long size)
{
    (void)addr; (void)size;        // no caches to worry about on the host
}

int seed_invoke(seed_entry fn, const BerryServices *svc,
                const berry_arg *argv, int argc, double *out_ret)
{
    (void)fn; (void)svc; (void)argv; (void)argc;
    *out_ret = 0;
    return -1;                     // native seeds run on the Pi, not the host
}

// PODs are AArch64 machine code too: the loader and its integrity checks work on
// the host (handy for testing), but actually entering one reports "unsupported".
int pod_run_main(const void *entry, const BerryServices *svc,
                 int argc, const char *const *argv, int *status)
{
    (void)entry; (void)svc; (void)argc; (void)argv;
    *status = 0;
    return -1;
}

int pod_run_kw(const void *entry, const BerryServices *svc,
               const berry_arg *argv, int argc, double *out)
{
    (void)entry; (void)svc; (void)argv; (void)argc;
    *out = 0;
    return -1;
}
