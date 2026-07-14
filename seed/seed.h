#ifndef SEED_H
#define SEED_H
// ---------------------------------------------------------------------------
// BerryBasiC native "seed" ABI.
//
// A *seed* is a small chunk of position-independent AArch64 machine code that a
// BASIC program can load from the SD card and call, for code that is too slow to
// interpret. It is the modern equivalent of BBC BASIC's CALL/USR to assembled
// machine code.
//
// A seed is built with the SAME bare-metal cross toolchain as the kernel
// (aarch64, -ffreestanding, -mcpu=cortex-a72), linked flat with seed/seed.ld, and
// objcopy'd to a raw ".sed" binary. Because all AArch64 control flow is
// PC-relative, a self-contained blob runs at whatever address it is loaded to;
// the build *fails* (see the Makefile 'seeds' target) if any relocation survives,
// which would mean the seed reached for libc / a global / data it cannot relocate.
//
// A seed never links against anything: everything it needs from the interpreter
// (console I/O, BASIC variables and arrays, returning a string) is reached through
// the SeedServices vtable passed in at entry. This is what keeps it self-contained.
//
// This one header is the single source of truth, shared by the interpreter, the
// platform backends, and every seed's source file.
// ---------------------------------------------------------------------------

#include <stdint.h>
#include <stddef.h>

#define SEED_MAGIC        0x44454553u   // 'S','E','E','D' little-endian
#define SEED_ABI_VERSION  6u            // v2 alloc; v3 realloc; v4 GPIO; v5 files; v6 fmt_num

// File open modes for the file_open service (match the storage layer). The seed
// <stdio.h> maps fopen's "r"/"w"/"a"/"r+"/... strings onto these.
#define SEED_FOPEN_READ   0            // existing file, read only        ("r")
#define SEED_FOPEN_WRITE  1            // create/truncate, read+write     ("w")
#define SEED_FOPEN_UPDATE 2            // existing file, read+write       ("r+")

// GPIO pin modes and pull settings, for the gpio_mode / gpio_pull services below
// (values match the interpreter's own gpio driver).
#define SEED_GPIO_IN        0
#define SEED_GPIO_OUT       1
#define SEED_GPIO_ALT       2
#define SEED_GPIO_PULL_NONE 0
#define SEED_GPIO_PULL_UP   1
#define SEED_GPIO_PULL_DOWN 2
// Edge argument for gpio_wait.
#define SEED_GPIO_FALLING   0
#define SEED_GPIO_RISING    1

// First bytes of a .sed file. The entry point sits at byte `entry_off` from the
// start of the blob (forced by seed.ld to be right after this header).
struct seed_header {
    uint32_t magic;       // SEED_MAGIC; rejects wrong-arch / stale files
    uint16_t version;     // SEED_ABI_VERSION the seed was built against
    uint16_t flags;       // reserved (0)
    uint32_t entry_off;   // entry point, bytes from blob start
    uint32_t reserved;    // pad to 16 bytes / future use
};

// One call argument: a BASIC number or a snapshot of a BASIC string. String
// bytes live in interpreter scratch and are valid only for the duration of the
// call; a seed that wants to keep one must copy it.
typedef struct {
    int          is_str;  // 0 = number (use .num), 1 = string (use .str/.len)
    double       num;
    const char  *str;     // not NUL-terminated; .len is authoritative
    int          len;
} seed_arg;

// Services the interpreter exposes to a seed. APPEND-ONLY: new members go on the
// end and `abi_version` is bumped, so seeds built against an older version keep
// working. All names are uppercase with their BASIC suffix, e.g. "X", "N%", "A$".
typedef struct SeedServices {
    uint32_t abi_version;

    // --- console -----------------------------------------------------------
    void (*putc)(int c);                      // write one character
    void (*puts)(const char *s, int len);     // write len bytes (no NUL needed)
    int  (*getkey)(void);                      // block for a key, return its code
    int  (*inkey)(int centiseconds);           // wait up to n cs; -1 on timeout

    // --- BASIC scalar variables -------------------------------------------
    int  (*get_num)(const char *name, double *out);   // 1 if defined, else 0
    void (*set_num)(const char *name, double val);
    int  (*get_str)(const char *name, char *buf, int buflen); // -> full length
    void (*set_str)(const char *name, const char *buf, int len);

    // --- numeric arrays: direct, zero-copy pointer (arr storage never moves) -
    double *(*num_array)(const char *name, int *out_len);     // 0 if not found

    // --- returning a string result (read by CALL$) ------------------------
    void (*set_return_str)(const char *buf, int len);

    // --- misc --------------------------------------------------------------
    uint32_t (*time_cs)(void);                 // centiseconds since boot

    // --- dynamic memory (ABI v2) ------------------------------------------
    // A seed heap, independent of BASIC's own storage. Memory persists across
    // calls within one RUN and is reclaimed wholesale at the next RUN/NEW, so a
    // seed that forgets to free will not leak across runs.
    void *(*alloc)(unsigned nbytes);           // 0 if the heap is exhausted
    void  (*free)(void *ptr);                  // free a prior alloc (0 = no-op)

    // --- dynamic memory (ABI v3) ------------------------------------------
    void *(*realloc)(void *ptr, unsigned nbytes);            // grow/shrink a block
    void *(*alloc_aligned)(unsigned alignment, unsigned nbytes);  // alignment = pow2

    // --- GPIO (ABI v4) -----------------------------------------------------
    // Direct access to the Raspberry Pi 4's 40-pin header (BCM numbering, pins
    // 0..27), for seeds that bit-bang a protocol or toggle pins faster than the
    // interpreter can. gpio_avail is 0 on the host build (everything else then
    // does nothing / reads 0), so a portable seed can check it first.
    int  (*gpio_avail)(void);                       // 1 on the Pi/QEMU, 0 on host
    int  (*gpio_mode)(int pin, int mode, int alt);  // SEED_GPIO_IN/OUT/ALT; <0 on bad pin
    int  (*gpio_pull)(int pin, int pull);           // SEED_GPIO_PULL_*; <0 on bad pin
    void (*gpio_write)(int pin, int level);         // drive an output 0/1
    int  (*gpio_read)(int pin);                     // read a pin -> 0/1
    void (*gpio_set)(uint32_t mask);                // set every 1-bit pin (atomic, GPSET0)
    void (*gpio_clr)(uint32_t mask);                // clear every 1-bit pin (atomic, GPCLR0)
    uint32_t (*gpio_level)(void);                   // all pin levels at once (low 28 bits)
    int  (*gpio_wait)(int pin, int edge, int timeout_cs);  // edge SEED_GPIO_*; pin or -1

    // --- SD-card files (ABI v5) -------------------------------------------
    // The minimal file interface the seed <stdio.h> is built on. These go
    // straight to the same filesystem BASIC uses (long file names included), so
    // a seed can read and write real files on the card. Handles are small
    // positive integers; 0 means failure. There are only a few channels in
    // total, shared with BASIC's OPEN* — a seed should close what it opens.
    int  (*file_open)(const char *name, int mode);       // SEED_FOPEN_*; handle>0 or 0
    int  (*file_close)(int fh);                          // 0 ok, <0 error
    int  (*file_read)(int fh, void *buf, int n);         // -> bytes read (0 = EOF), <0 error
    int  (*file_write)(int fh, const void *buf, int n);  // -> bytes written, <0 error
    long (*file_seek)(int fh, long off, int whence);     // whence 0/1/2 -> new pos, <0 error
    long (*file_size)(int fh);                           // length in bytes, <0 error
    int  (*file_eof)(int fh);                            // 1 at end-of-file, else 0
    int  (*file_remove)(const char *name);              // delete a file; 0 ok, <0 error

    // --- number formatting (ABI v6) ---------------------------------------
    // Format a double into `out` (needs >= 32 bytes) the way BASIC's PRINT does
    // (up to 9 significant digits, fixed-point or E-notation as appropriate,
    // "NAN"/"INF" for those); returns the number of characters written. This is
    // what the seed <stdio.h> uses for the printf %f/%e/%g conversions, so seed
    // and BASIC number output match exactly.
    int  (*fmt_num)(double v, char *out);
} SeedServices;

// A seed's entry point. Returns a number (also usable as a status); a string
// result is returned via svc->set_return_str and read with CALL$.
typedef double (*seed_entry)(const SeedServices *svc, const seed_arg *argv, int argc);

// ---------------------------------------------------------------------------
// Platform hooks: implemented by the target (seed/seed_target.c) and the host
// (seed/seed_host.c). The interpreter (basic/basic.c) calls these.
// ---------------------------------------------------------------------------

// Make freshly-loaded bytes executable: clean them out of the D-cache to the
// point of unification and invalidate the matching I-cache lines. MUST be called
// after copying a seed into RAM and before calling it.
void icache_sync(const void *addr, unsigned long size);

// Transfer control to a loaded seed. Returns 0 on success (and stores the seed's
// numeric result in *out_ret), or -1 if native seeds are unsupported on this
// build (the host backend, which cannot run AArch64 code).
int  seed_invoke(seed_entry fn, const SeedServices *svc,
                 const seed_arg *argv, int argc, double *out_ret);

// ---------------------------------------------------------------------------
// Seed-author conveniences.
//
// A seed gets a small freestanding C library (its own <stdlib.h>, <string.h>,
// <ctype.h>) implemented by the seed runtime, so familiar standard functions
// like malloc/qsort/strlen work instead of reaching through `svc`. The entry
// trampoline (below) stashes the services pointer in `seed_svc`, which the
// runtime's malloc/free/etc. route through.
// ---------------------------------------------------------------------------
extern const SeedServices *seed_svc;   // set on entry; used by the seed libc

// ---------------------------------------------------------------------------
// Helper for seed source files: declares the header in its own section (placed
// first by seed.ld) and opens the entry function (forced right after it, so the
// header's entry_off == sizeof(struct seed_header)). The exported entry is a
// tiny trampoline that captures `svc` for malloc/free, then runs the body the
// author writes after the macro.
//
//   #include "seed.h"
//   SEED_EXPORT(myseed) { return argv[0].num + argv[1].num; }
// ---------------------------------------------------------------------------
#define SEED_EXPORT(name)                                                      \
    static double name##_body(const SeedServices *svc,                         \
                              const seed_arg *argv, int argc);                 \
    static const struct seed_header                                            \
        __attribute__((section(".seed.header"), used)) seed_hdr_ = {           \
            SEED_MAGIC, (uint16_t)SEED_ABI_VERSION, 0,                         \
            (uint32_t)sizeof(struct seed_header), 0 };                         \
    __attribute__((section(".seed.entry"), used))                              \
    double name(const SeedServices *svc, const seed_arg *argv, int argc) {     \
        seed_svc = svc;                                                        \
        return name##_body(svc, argv, argc);                                   \
    }                                                                          \
    static double name##_body(const SeedServices *svc,                         \
                              const seed_arg *argv, int argc)

#endif // SEED_H
