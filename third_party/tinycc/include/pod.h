#ifndef POD_H
#define POD_H
/* ==========================================================================
 * BerryBasiC POD executable format — author runtime header.
 *
 * A POD is a self-contained native program for the BerryBasiC machine: one
 * flat, position-independent image split once into read-execute and read-write
 * halves, wrapped with a checksummed header, a plain-text provenance record and
 * a declared set of capabilities.  See doc "The POD Executable Format".
 *
 * Build a POD straight from C with the BerryBasiC tcc:
 *
 *     tcc -pod hello.c -o HELLO.POD
 *
 * This header is what a POD's source #includes.  It provides:
 *   - the BerryServices table the loader hands the program at entry,
 *   - the capability bits, and
 *   - the manifest macros (POD_NAME/POD_NEEDS/...) that record, in a dedicated
 *     ".pod.desc" section, what the wrapper should stamp into the file.
 *
 * The compiler never links a POD against anything: everything the program needs
 * from the interpreter is reached through the services table (BerryServices,
 * the same table a seed gets).  A capability the POD did not declare is a
 * refusal stub in that table, so there is nothing to bypass.
 * ========================================================================== */

typedef unsigned char      pod_u8;
typedef unsigned short     pod_u16;
typedef unsigned int       pod_u32;
typedef unsigned long long pod_u64;

/* The services table (BerryServices), argument struct (berry_arg) and ABI version
 * (BERRY_ABI_VERSION) live once in berry_services.h, shared by seeds and PODs. */
#include "berry_services.h"

/* -------------------------------------------------------------------------- *
 * Capability bits (the header's `caps` mask, and the argument to POD_NEEDS).
 * The loader builds a services table containing only the groups granted here;
 * every other slot is a refusal stub.  Enforcement is structural, not a check.
 * -------------------------------------------------------------------------- */
#define CAP_CONSOLE  (1ull << 0)   /* puts/putc/getkey/inkey                   */
#define CAP_VARS     (1ull << 1)   /* read/write BASIC scalars and arrays      */
#define CAP_FILES    (1ull << 2)   /* open/read/write/delete files             */
#define CAP_DIRS     (1ull << 3)   /* create/remove/traverse directories       */
#define CAP_GRAPHICS (1ull << 4)   /* all gfx_* drawing                        */
#define CAP_SOUND    (1ull << 5)   /* tone and sample playback                 */
#define CAP_GPIO     (1ull << 6)   /* pin mode/read/write                      */
#define CAP_I2C      (1ull << 7)   /* bus transactions                         */
#define CAP_TIME     (1ull << 8)   /* clock and timing                         */
#define CAP_HEAP     (1ull << 9)   /* allocate from the pod heap               */
#define CAP_KEYWORD  (1ull << 10)  /* register language keywords               */
#define CAP_SPAWN    (1ull << 11)  /* load and run other PODs                  */
#define CAP_RAWMEM   (1ull << 12)  /* read/write memory outside its own image  */
#define CAP_CORES    (1ull << 13)  /* dispatch work to worker cores            */
#define CAP_NET      (1ull << 14)  /* reserved for networking                  */

/* -------------------------------------------------------------------------- *
 * Header flags (POD_KIND is bit 0 of the header `flags`).
 * -------------------------------------------------------------------------- */
#define POD_KIND_PROGRAM   0   /* has pod_main                                 */
#define POD_KIND_EXTENSION 1   /* registers keywords, stays resident           */

/* The services table the loader hands a POD at entry is BerryServices,
 * defined once in berry_services.h (included above) and shared with seeds.
 * Groups the POD did not request via POD_NEEDS are refusal stubs. */

/* A program POD's entry point.  Returns the exit status: 0 = success. */
typedef int (*pod_main_fn)(const BerryServices *svc,
                           int argc, const char *const *argv);

/* An extension POD's keyword handlers (registered via KEYW). */
typedef double (*pod_kw_num_fn)(const BerryServices *svc,
                                const berry_arg *argv, int argc);
typedef void   (*pod_kw_stmt_fn)(const BerryServices *svc,
                                 const berry_arg *argv, int argc);

/* Keyword kinds, for POD_KEYWORD. */
#define POD_KW_STATEMENT 0   /* NAME arg,arg            (return ignored)       */
#define POD_KW_NUMFN     1   /* x = NAME(arg,arg)                              */
#define POD_KW_STRFN     2   /* a$ = NAME$(arg,arg)     (via set_return_str)   */

/* ==========================================================================
 * Manifest macros.
 *
 * Each macro drops a small self-describing record into the ".pod.desc"
 * section.  The wrapper reads them all to fill the header's `caps`/`flags`,
 * the MARK provenance record and the NEED rationale.  Records are packed and
 * byte-aligned so they sit contiguously; each is  {u16 tag; u16 len; bytes}.
 * ========================================================================== */
#define POD__T_NAME  1   /* text: program name                                */
#define POD__T_VERS  2   /* text: version string                              */
#define POD__T_AUTH  3   /* text: author                                      */
#define POD__T_DESC  4   /* text: one-line description                        */
#define POD__T_LICE  5   /* text: licence                                     */
#define POD__T_CAPS  6   /* u64:  capability bits to OR into the mask          */
#define POD__T_NEED  7   /* text: one "CAP=reason" rationale line             */
#define POD__T_FLAGS 8   /* u16:  header flags to OR in (kind, self-mod, ...) */
#define POD__T_ABI   9   /* u16:  minimum BerryServices ABI required            */
#define POD__T_KEYW 10   /* keyword registration (extension PODs)             */

#define POD__CAT(a, b)  a##b
#define POD__CAT2(a, b) POD__CAT(a, b)

#define POD__SECTION __attribute__((used, section(".pod.desc"), aligned(1)))

/* Each record is placed via a NAMED struct type, uniquified with __COUNTER__.
 * (tcc honours a `section` attribute on a named struct's declarator but drops
 * it on an anonymous one, so a named type per record is what lands the manifest
 * in .pod.desc.)
 *
 * Every field is a byte (pod_u8), so each record struct has alignment 1: the
 * compiler never pads within a record, and never inserts padding BETWEEN
 * records in the section.  That keeps the whole manifest a tight stream the
 * wrapper walks by stepping (4 + len) each time.  A record is:
 *
 *      byte 0..1  tag   (little-endian u16)
 *      byte 2..3  len   (little-endian u16) = payload bytes that follow
 *      byte 4..   payload
 *
 * `len` is computed as sizeof(record) - 4 so it always matches the stored size;
 * text payloads are NUL-terminated within it. */
#define POD__HDR(tag, styp, c)                                                \
    (pod_u8)(tag), (pod_u8)((tag) >> 8),                                      \
    (pod_u8)(sizeof(struct POD__CAT(styp, c)) - 4),                           \
    (pod_u8)((sizeof(struct POD__CAT(styp, c)) - 4) >> 8)

#define POD__STR(tag, s)     POD__STR_(tag, s, __COUNTER__)
#define POD__STR_(tag, s, c) POD__STR__(tag, s, c)
#define POD__STR__(tag, s, c)                                                 \
    struct POD__CAT(pod__ts, c) { pod_u8 h[4]; char d[sizeof(s)]; };          \
    static const struct POD__CAT(pod__ts, c) POD__SECTION                     \
        POD__CAT(pod__vs, c) = { { POD__HDR(tag, pod__ts, c) }, s }

/* 64-bit record: eight little-endian bytes. */
#define POD__U64(tag, v)     POD__U64_(tag, v, __COUNTER__)
#define POD__U64_(tag, v, c) POD__U64__(tag, v, c)
#define POD__U64__(tag, v, c)                                                 \
    struct POD__CAT(pod__tq, c) { pod_u8 h[4]; pod_u8 d[8]; };                \
    static const struct POD__CAT(pod__tq, c) POD__SECTION                     \
        POD__CAT(pod__vq, c) = { { POD__HDR(tag, pod__tq, c) }, {             \
            (pod_u8)((pod_u64)(v)),       (pod_u8)((pod_u64)(v) >> 8),        \
            (pod_u8)((pod_u64)(v) >> 16), (pod_u8)((pod_u64)(v) >> 24),       \
            (pod_u8)((pod_u64)(v) >> 32), (pod_u8)((pod_u64)(v) >> 40),       \
            (pod_u8)((pod_u64)(v) >> 48), (pod_u8)((pod_u64)(v) >> 56) } }

/* 16-bit record: two little-endian bytes. */
#define POD__U16(tag, v)     POD__U16_(tag, v, __COUNTER__)
#define POD__U16_(tag, v, c) POD__U16__(tag, v, c)
#define POD__U16__(tag, v, c)                                                 \
    struct POD__CAT(pod__tw, c) { pod_u8 h[4]; pod_u8 d[2]; };                \
    static const struct POD__CAT(pod__tw, c) POD__SECTION                     \
        POD__CAT(pod__vw, c) = { { POD__HDR(tag, pod__tw, c) },               \
            { (pod_u8)(v), (pod_u8)((v) >> 8) } }

/* --- the public manifest vocabulary --- *
 * These are self-terminating declarations: write them one per line, WITHOUT a
 * trailing semicolon, exactly as in the doc's examples. */
#define POD_NAME(s)        POD__STR(POD__T_NAME, s);
#define POD_VERSION(s)     POD__STR(POD__T_VERS, s);
#define POD_AUTHOR(s)      POD__STR(POD__T_AUTH, s);
#define POD_DESCRIPTION(s) POD__STR(POD__T_DESC, s);
#define POD_LICENSE(s)     POD__STR(POD__T_LICE, s);
#define POD_ABI(n)         POD__U16(POD__T_ABI, n);
#define POD_FLAGS(n)       POD__U16(POD__T_FLAGS, n);

/* Declare a capability and, in the same breath, why it is wanted.  May appear
 * more than once; the wrapper ORs the masks and keeps every rationale line. */
#define POD_NEEDS(capmask, reason)                                            \
    POD__U64(POD__T_CAPS, (capmask));                                         \
    POD__STR(POD__T_NEED, reason);

/* Register a BASIC keyword implemented by `handler` (an extension POD).  The
 * wrapper resolves the handler's image offset by symbol name and marks the
 * POD as an extension.  Fields are byte/char aligned, so the record is
 * contiguous with no padding. */
#define POD_KEYWORD(kwname, kwkind, kwmin, kwmax, handler)                    \
    POD__KW_(kwname, kwkind, kwmin, kwmax, handler, __COUNTER__)
#define POD__KW_(kwname, kwkind, kwmin, kwmax, handler, c)                    \
    POD__KW__(kwname, kwkind, kwmin, kwmax, handler, c)
#define POD__KW__(kwname, kwkind, kwmin, kwmax, handler, c)                   \
    struct POD__CAT(pod__tk, c) { pod_u8 h[4]; char name[12];                \
        pod_u8 kind, mn, mx, flags; char sym[20]; };                          \
    static const struct POD__CAT(pod__tk, c) POD__SECTION                     \
        POD__CAT(pod__vk, c) = { { POD__HDR(POD__T_KEYW, pod__tk, c) },       \
        kwname, (pod_u8)(kwkind), (pod_u8)(kwmin), (pod_u8)(kwmax), 0, #handler };

#endif /* POD_H */
