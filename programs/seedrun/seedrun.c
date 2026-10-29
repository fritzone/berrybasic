// seedrun - a POD that loads a native seed (.SED) and runs it.
//
//   seedrun ADD.SED 40 2      => 42
//   seedrun HYPOT.SED 3 4     => 5
//   seedrun DYNARR.SED 100    => 5050
//   seedrun BGIDEMO.SED       (draws the graphics tour)
//
// A seed is a flat, position-independent AArch64 blob (see seed.h). This POD
// reads it, checks the header, copies it into a page-aligned block, makes that
// block executable (icache_sync - the D- and I-caches are not coherent on ARM),
// and calls the seed's entry point with the numbers from the command line.
//
// The seed is handed THIS pod's services table, so it runs with the pod's
// capabilities: a seed that only computes needs nothing, one that draws needs
// the pod to have declared CAP_GRAPHICS, and so on. seedrun declares a broad set
// and lends them all. (A seed that reaches BASIC variables by name finds none -
// there is no BASIC program here - so seedrun best suits the compute and drawing
// seeds, e.g. ADD, HYPOT, DYNARR, BGIDEMO.)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pod.h>
#include <seed.h>

POD_NAME("seedrun")
POD_DESCRIPTION("load a native seed and run it")
POD_NEEDS(CAP_CONSOLE | CAP_FILES | CAP_HEAP | CAP_VARS | CAP_DIRS |
          CAP_GRAPHICS | CAP_SOUND | CAP_GPIO | CAP_I2C | CAP_TIME,
          "CONSOLE=the result; FILES=read the .SED; HEAP=executable memory; "
          "the rest are lent to the seed that runs")

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: seedrun FILE.SED [number ...]\n");
        return 1;
    }
    const char *path = argv[1];

    // Try the path as given, then /seed (the home for installed seeds), so a bare
    // "ADD.SED" is found from anywhere - the same rule the SEED statement uses.
    FILE *f = fopen(path, "rb");
    if (!f) {
        int has_slash = 0;
        for (const char *p = path; *p; p++) if (*p == '/') has_slash = 1;
        if (!has_slash) {
            char alt[80];
            snprintf(alt, sizeof alt, "/seed/%s", path);
            f = fopen(alt, "rb");
        }
    }
    if (!f) { printf("seedrun: cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz < (long)sizeof(struct seed_header)) {
        printf("seedrun: %s is not a seed\n", path); fclose(f); return 1;
    }

    struct seed_header hdr;
    if (fread(&hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return 1; }
    if (hdr.magic != SEED_MAGIC) {
        printf("seedrun: %s is not a seed (bad magic)\n", path); fclose(f); return 1;
    }
    if (hdr.version > BERRY_ABI_VERSION) {
        printf("seedrun: %s was built for a newer host (abi %u)\n", path, hdr.version);
        fclose(f); return 1;
    }
    if (hdr.entry_off >= (unsigned)fsz) {
        printf("seedrun: %s has a bad entry offset\n", path); fclose(f); return 1;
    }

    // The seed's full in-memory footprint (code + .bss). tcc -seed stamps
    // image_size; a gcc-built seed leaves it 0, so keep a .bss margin. Page-align
    // it: a seed uses PC-relative ADRP+ADD, which needs a 4 KiB-aligned base.
    unsigned foot = hdr.image_size ? hdr.image_size : (unsigned)fsz + 16384u;
    if (foot < (unsigned)fsz) foot = (unsigned)fsz;
    foot = (foot + 4095u) & ~4095u;

    unsigned char *mem = (unsigned char *)berry_svc->alloc_aligned(4096, foot);
    if (!mem) { printf("seedrun: out of memory\n"); fclose(f); return 1; }
    memset(mem, 0, foot);                                  // zero the .bss tail
    fseek(f, 0, SEEK_SET);
    if (fread(mem, 1, (size_t)fsz, f) != (size_t)fsz) {
        printf("seedrun: short read\n"); fclose(f); return 1;
    }
    fclose(f);

    berry_svc->icache_sync(mem, (unsigned long)fsz);       // make the bytes runnable

    // The numbers after the filename become the seed's arguments.
    berry_arg av[16];
    int ac = 0;
    for (int i = 2; i < argc && ac < 16; i++) {
        av[ac].is_str = 0;
        av[ac].num    = atof(argv[i]);
        av[ac].str    = 0;
        av[ac].len    = 0;
        ac++;
    }

    seed_entry entry = (seed_entry)(void *)(mem + hdr.entry_off);
    double r = entry(berry_svc, av, ac);

    if (r == (double)(long)r) printf("=> %ld\n", (long)r);   // a whole number, tidily
    else                      printf("=> %f\n", r);
    return 0;
}
