// Target (AArch64 bare-metal) backend for native seeds: make loaded bytes
// executable, and transfer control to a seed. See seed/seed.h for the ABI.
#include "seed.h"

// Clean the loaded bytes out of the D-cache to the point of unification, then
// invalidate the matching I-cache lines, so the core fetches the new code rather
// than stale instructions. D- and I-caches are not coherent on ARM, so this is
// mandatory after writing code into RAM (the seed arrives via the SD/FAT path,
// i.e. as ordinary data writes).
void icache_sync(const void *addr, unsigned long size)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t dline = 4UL << ((ctr >> 16) & 0xF);   // CTR_EL0.DminLine (words)
    uint64_t iline = 4UL << (ctr & 0xF);           // CTR_EL0.IminLine (words)

    uint64_t start = (uint64_t)(uintptr_t)addr;
    uint64_t end   = start + size;

    for (uint64_t p = start & ~(dline - 1); p < end; p += dline)
        __asm__ volatile("dc cvau, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish");

    for (uint64_t p = start & ~(iline - 1); p < end; p += iline)
        __asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");
}

// Call the seed. It runs synchronously on the current EL1 stack and returns
// normally; a faulting seed is caught by the kernel's exception vectors.
int seed_invoke(seed_entry fn, const SeedServices *svc,
                const seed_arg *argv, int argc, double *out_ret)
{
    *out_ret = fn(svc, argv, argc);
    return 0;
}
