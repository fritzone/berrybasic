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
// the BerryServices vtable passed in at entry. This is what keeps it self-contained.
//
// This one header is the single source of truth, shared by the interpreter, the
// platform backends, and every seed's source file.
// ---------------------------------------------------------------------------

#include <stdint.h>
#include <stddef.h>
#include "berry_services.h"   // BerryServices, berry_arg, BERRY_ABI_VERSION

#define SEED_MAGIC        0x44454553u   // 'S','E','E','D' little-endian

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

// Modifier and lock bits returned by the keymods service (values match the
// KMOD_* set in drivers/usb_hid.h, which a seed does not include). Left and
// right of a pair are folded together; ALTGR is the right-hand Alt, which types
// the third legend on Nordic/German boards.
#define SEED_KMOD_SHIFT  0x001
#define SEED_KMOD_CTRL   0x002
#define SEED_KMOD_ALT    0x004
#define SEED_KMOD_ALTGR  0x008
#define SEED_KMOD_META   0x010   // the Windows / Command key
#define SEED_KMOD_CAPS   0x020   // lock states
#define SEED_KMOD_NUM    0x040
#define SEED_KMOD_SCROLL 0x080

// Keys that type nothing, as returned by getkey/inkey (these match the KEY_*
// set in drivers/usb_hid.h). Printable keys return their Latin-1 character, so
// everything added here sits above 0xFF to stay clear of it.
#define SEED_KEY_LEFT   0x11
#define SEED_KEY_RIGHT  0x12
#define SEED_KEY_UP     0x13
#define SEED_KEY_DOWN   0x14
#define SEED_KEY_HOME   0x15
#define SEED_KEY_END    0x16
#define SEED_KEY_INS    0x17
#define SEED_KEY_ESC    0x1B
#define SEED_KEY_DEL    0x7F
#define SEED_KEY_F1     0x101    // F1..F12 are consecutive
#define SEED_KEY_F12    0x10C
#define SEED_KEY_F(n)   (SEED_KEY_F1 + (n) - 1)
#define SEED_KEY_PGUP   0x10D
#define SEED_KEY_PGDN   0x10E

// Header flags (the `flags` field below).
#define SEED_HDR_KEYWORD  0x0001u   // this seed registers a language keyword (see
                                    // SEED_KEYWORD): a seed_keyword descriptor
                                    // follows the header, and entry_off is past it.

// First bytes of a .sed file. The entry point sits at byte `entry_off` from the
// start of the blob (forced by seed.ld to be right after this header, or right
// after the optional seed_keyword descriptor when SEED_HDR_KEYWORD is set).
struct seed_header {
    uint32_t magic;       // SEED_MAGIC; rejects wrong-arch / stale files
    uint16_t version;     // BERRY_ABI_VERSION the seed was built against
    uint16_t flags;       // SEED_HDR_* (0 for a plain seed)
    uint32_t entry_off;   // entry point, bytes from blob start
    uint32_t reserved;    // pad to 16 bytes / future use
};

// How a registered keyword is used from BASIC (the `kind` field below):
#define SEED_KW_STATEMENT 0   // a command:            NAME arg, arg       (return ignored)
#define SEED_KW_NUMFN     1   // a numeric function:   x = NAME(arg, arg)
#define SEED_KW_STRFN     2   // a string function:    a$ = NAME$(arg, arg) (result via set_return_str)

// A keyword a seed adds to the language. When SEED_HDR_KEYWORD is set this
// descriptor sits immediately after the header (at offset sizeof(seed_header)),
// so the interpreter can read it while scanning the /seed directory at startup
// and wire NAME straight into the lexer — no SEED/CALL needed. The seed's entry
// point is the keyword's implementation; the gathered arguments arrive as argv[].
struct seed_keyword {
    char     name[16];    // the keyword AS TYPED, uppercase, incl. a trailing '$'
                          //   for a string function (e.g. "BOX", "HYPOT", "REV$")
    uint16_t kind;        // SEED_KW_STATEMENT / SEED_KW_NUMFN / SEED_KW_STRFN
    uint16_t min_args;    // fewest / most arguments accepted (inclusive). The
    uint16_t max_args;    //   interpreter checks the count before calling.
    uint16_t reserved;
    uint32_t pad[2];      // -> 32 bytes total. Kept a multiple of 16 so the entry
                          //   that follows (functions are 16-aligned) lands right
                          //   after with no linker padding, i.e. entry_off ==
                          //   sizeof(header)+sizeof(keyword). See seed.ld.
};

// Call arguments arrive as berry_arg[] (see berry_services.h).

// The services table the interpreter hands a seed is BerryServices, defined
// once in berry_services.h and shared with PODs.

// A seed's entry point. Returns a number (also usable as a status); a string
// result is returned via svc->set_return_str and read with CALL$.
typedef double (*seed_entry)(const BerryServices *svc, const berry_arg *argv, int argc);

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
int  seed_invoke(seed_entry fn, const BerryServices *svc,
                 const berry_arg *argv, int argc, double *out_ret);

// ---------------------------------------------------------------------------
// Seed-author conveniences.
//
// A seed gets a small freestanding C library (its own <stdlib.h>, <string.h>,
// <ctype.h>) implemented by the seed runtime, so familiar standard functions
// like malloc/qsort/strlen work instead of reaching through `svc`. The entry
// trampoline (below) stashes the services pointer in `seed_svc`, which the
// runtime's malloc/free/etc. route through.
// ---------------------------------------------------------------------------
extern const BerryServices *seed_svc;   // set on entry; used by the seed libc

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
    static double name##_body(const BerryServices *svc,                         \
                              const berry_arg *argv, int argc);                 \
    static const struct seed_header                                            \
        __attribute__((section(".seed.header"), used)) seed_hdr_ = {           \
            SEED_MAGIC, (uint16_t)BERRY_ABI_VERSION, 0,                         \
            (uint32_t)sizeof(struct seed_header), 0 };                         \
    __attribute__((section(".seed.entry"), used))                              \
    double name(const BerryServices *svc, const berry_arg *argv, int argc) {     \
        seed_svc = svc;                                                        \
        return name##_body(svc, argv, argc);                                   \
    }                                                                          \
    static double name##_body(const BerryServices *svc,                         \
                              const berry_arg *argv, int argc)

// ---------------------------------------------------------------------------
// SEED_KEYWORD — register a new BASIC keyword.
//
// Like SEED_EXPORT, but the seed also carries a seed_keyword descriptor, so at
// startup the interpreter scans /seed, finds this seed, and adds `kwname` to the
// language. The keyword is then used directly — no SEED/CALL:
//
//   SEED_KEYWORD("HYPOT", SEED_KW_NUMFN, 2, 2) {        // r = HYPOT(3, 4)
//       double a = argv[0].num, b = argv[1].num;
//       return sqrt(a*a + b*b);
//   }
//
//   SEED_KEYWORD("SHOUT", SEED_KW_STATEMENT, 1, 1) {    // SHOUT "hi"
//       for (int i = 0; i < argv[0].len; i++) svc->putc(toupper(argv[0].str[i]));
//       svc->putc('\n');
//       return 0;
//   }
//
// A string function (SEED_KW_STRFN) returns its text through svc->set_return_str
// and is named with a trailing '$'. `kwmin`/`kwmax` bound the argument count.
// Only seeds that use this macro become keywords; SEED_EXPORT seeds stay plain.
// ---------------------------------------------------------------------------
#define SEED_KEYWORD(kwname, kwkind, kwmin, kwmax)                             \
    static double seed_kw_body(const BerryServices *svc,                       \
                               const berry_arg *argv, int argc);               \
    static const struct seed_header                                           \
        __attribute__((section(".seed.header"), used)) seed_hdr_ = {          \
            SEED_MAGIC, (uint16_t)BERRY_ABI_VERSION, SEED_HDR_KEYWORD,         \
            (uint32_t)(sizeof(struct seed_header) +                           \
                       sizeof(struct seed_keyword)), 0 };                     \
    static const struct seed_keyword                                          \
        __attribute__((section(".seed.keyword"), used)) seed_kw_desc_ = {     \
            kwname, (uint16_t)(kwkind), (uint16_t)(kwmin), (uint16_t)(kwmax), 0 };\
    __attribute__((section(".seed.entry"), used))                            \
    double seed_kw_entry(const BerryServices *svc,                            \
                         const berry_arg *argv, int argc) {                    \
        seed_svc = svc;                                                       \
        return seed_kw_body(svc, argv, argc);                                 \
    }                                                                         \
    static double seed_kw_body(const BerryServices *svc,                       \
                               const berry_arg *argv, int argc)

#endif // SEED_H
