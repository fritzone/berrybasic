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
#define SEED_ABI_VERSION 11u            // v2 alloc; v3 realloc; v4 GPIO; v5 files; v6 fmt_num;
                                        // v7 graphics; v8 records; v9 mouse; v10 double
                                        // buffering; v11 keyboard modifiers

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
    uint16_t version;     // SEED_ABI_VERSION the seed was built against
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

    // --- graphics (ABI v7) -------------------------------------------------
    // Physical-pixel drawing straight onto the framebuffer the seed's <graphics.h>
    // (a BGI-style library) is built on. Coordinates are device pixels with the
    // origin at the TOP-LEFT (x right, y down), NOT BASIC's logical bottom-left
    // system; colours are 24-bit 0xRRGGBB. gfx_avail is 0 on the host build (and
    // every draw call is then a no-op), so a portable seed checks it first.
    // Drawing goes to whatever surface BASIC is currently targeting (the visible
    // screen, or a BUFFER/SPRITETARGET), so a seed composes with BASIC graphics.
    int  (*gfx_avail)(void);                          // 1 with a framebuffer, else 0
    int  (*gfx_width)(void);                          // screen width in pixels
    int  (*gfx_height)(void);                         // screen height in pixels
    void (*gfx_clear)(uint32_t rgb);                  // fill the whole surface
    void (*gfx_putpixel)(int x, int y, uint32_t rgb);
    uint32_t (*gfx_getpixel)(int x, int y);           // 0xRRGGBB at (x,y)
    void (*gfx_line)(int x1, int y1, int x2, int y2, uint32_t rgb);
    void (*gfx_fillrect)(int x1, int y1, int x2, int y2, uint32_t rgb);
    void (*gfx_circle)(int cx, int cy, int r, uint32_t rgb);         // outline
    void (*gfx_fillcircle)(int cx, int cy, int r, uint32_t rgb);
    void (*gfx_ellipse)(int cx, int cy, int rx, int ry, uint32_t rgb);   // outline
    void (*gfx_fillellipse)(int cx, int cy, int rx, int ry, uint32_t rgb);
    void (*gfx_fillpoly)(const int *xy, int npts, uint32_t rgb);     // xy = x0,y0,x1,y1,...
    void (*gfx_flood)(int x, int y, uint32_t rgb);    // flood the region under (x,y)
    void (*gfx_clip)(int x1, int y1, int x2, int y2); // restrict drawing to a rectangle
    void (*gfx_noclip)(void);                         // remove the clip rectangle

    // --- text: TrueType fonts (ABI v7) ------------------------------------
    // Load and draw TrueType text (the same engine BASIC's GTEXT uses). The
    // metric calls work even on the host (no framebuffer needed); gfx_text is a
    // no-op there. Handles are 1..N; font_load returns 0 on failure.
    int  (*font_load)(const char *name);              // load a .ttf -> handle, or 0
    int  (*font_select)(int handle);                  // 1 ok, 0 unknown handle
    void (*font_size)(int pixels);                    // glyph height in pixels
    void (*font_style)(int bold, int italic, int underline);   // 0/1 flags
    void (*gfx_text)(int x, int y, const char *s, int len, uint32_t rgb); // baseline at (x,y)
    int  (*text_width)(const char *s, int len);       // pixel width in the current font
    int  (*text_height)(void);                        // line height in pixels

    // --- records (ABI v8) --------------------------------------------------
    // BASIC's user-defined types (TYPE point : x, y : ENDTYPE / DIM e(99) AS
    // point). A record is reached by *name*, exactly like a numeric array - it
    // is never passed in as an argument.
    //
    // The numeric fields of one element are stored contiguously, so a record -
    // or a whole array of records - is a strided array of doubles, and
    // rec_array hands a seed a direct pointer to it. Element `e`, field `f` is
    //
    //     base[e * stride + f]        f from rec_field(), 0 <= e < nelem
    //
    // which is what makes "update 10,000 particles" worth writing as a seed.
    // Text fields are NOT in that block (the interpreter moves string bytes
    // around), so they are copied in and out by name, like any other string.

    // Pointer to element 0's numeric fields; *nelem elements of *stride doubles
    // each. 0 (with *nelem = 0) if `name` is not a record. The pool never moves,
    // so the pointer stays valid for the whole RUN.
    double *(*rec_array)(const char *name, int *nelem, int *stride);

    // Index of a numeric field within one element, for use with rec_array().
    // -1 if there is no such field, or if it is a text field.
    int  (*rec_field)(const char *name, const char *field);

    // Text fields, copied. get returns the full length even if truncated (like
    // get_str); both are no-ops/0 if the record, element or field is unknown.
    int  (*rec_get_str)(const char *name, int elem, const char *field,
                        char *buf, int buflen);
    void (*rec_set_str)(const char *name, int elem, const char *field,
                        const char *buf, int len);

    // --- pointer / mouse (ABI v9) ------------------------------------------
    // Poll the USB mouse and report the pointer: position in *raw framebuffer
    // pixels* (origin top-left - the same space the gfx_* calls draw in, and
    // the same numbers BASIC's MOUSEX/MOUSEY report) and a button bitmask
    // (bit0 = left, bit1 = right, bit2 = middle). Any pointer may be 0. With no
    // mouse present, or on the host build, everything reads back 0.
    //
    // The call *is* the poll. The interpreter services the mouse between BASIC
    // statements, so while a seed is running nothing else is moving the
    // pointer: a seed that wants a live mouse must call this itself, in its own
    // loop. Keyboard input is the pair above (getkey / inkey), which behave
    // exactly like BASIC's GET and INKEY.
    void (*mouse)(int *x, int *y, int *buttons);

    // --- double buffering (ABI v10) ----------------------------------------
    // Draw the next frame off-screen and present it in one go, so a redrawing
    // loop never shows a half-finished screen. This is the same mechanism as
    // BASIC's BUFFER ON / FLIP, and the *same state*: a seed that turns it on
    // should put the setting back as it found it (gfx_buffered() reports it),
    // or a program that was buffering its own graphics will stop working.
    //
    // gfx_backbuffer(1) routes every drawing call - the gfx_* above, and the
    // seed's <graphics.h> on top of them - to an off-screen buffer; the visible
    // screen then holds still until gfx_flip() copies the buffer onto it. The
    // buffer keeps its contents across flips, so a loop is free to redraw only
    // the parts that changed. Returns 0 on success, <0 if the buffer could not
    // be allocated (the screen is then simply undoubled - drawing still works).
    int  (*gfx_backbuffer)(int on);
    void (*gfx_flip)(void);
    int  (*gfx_buffered)(void);

    // --- keyboard modifiers and locks (ABI v11) ----------------------------
    // Which modifiers are held and which locks are set, as a bitmask of the
    // SEED_KMOD_* values above. getkey/inkey cannot tell you this:
    // Shift on its own types nothing, so it is state rather than a key.
    //
    // Like the mouse, it is a snapshot refreshed when the keyboard is read, so
    // call inkey(0) first in a polling loop or you get however stale the last
    // read left it. 0 when no USB keyboard is present.
    int  (*keymods)(void);
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
    static double seed_kw_body(const SeedServices *svc,                       \
                               const seed_arg *argv, int argc);               \
    static const struct seed_header                                           \
        __attribute__((section(".seed.header"), used)) seed_hdr_ = {          \
            SEED_MAGIC, (uint16_t)SEED_ABI_VERSION, SEED_HDR_KEYWORD,         \
            (uint32_t)(sizeof(struct seed_header) +                           \
                       sizeof(struct seed_keyword)), 0 };                     \
    static const struct seed_keyword                                          \
        __attribute__((section(".seed.keyword"), used)) seed_kw_desc_ = {     \
            kwname, (uint16_t)(kwkind), (uint16_t)(kwmin), (uint16_t)(kwmax), 0 };\
    __attribute__((section(".seed.entry"), used))                            \
    double seed_kw_entry(const SeedServices *svc,                            \
                         const seed_arg *argv, int argc) {                    \
        seed_svc = svc;                                                       \
        return seed_kw_body(svc, argv, argc);                                 \
    }                                                                         \
    static double seed_kw_body(const SeedServices *svc,                       \
                               const seed_arg *argv, int argc)

#endif // SEED_H
