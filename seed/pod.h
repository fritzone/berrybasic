#ifndef POD_H
#define POD_H
// ===========================================================================
// BerryBasiC POD executable — loader-side ABI.
//
// A *POD* ("what holds seeds") is a self-contained native program: one flat,
// position-independent image split once into read-execute and read-write halves,
// wrapped by a 64-byte checksummed header and a run of forward-only chunks
// (MARK/NEED/IMAG/KEYW/RLOC/NOTE/SEAL).  It generalises a single native seed (see
// seed.h) into a whole capability-scoped program or vocabulary.
//
// This header is the loader's view of the format: the header layout, the chunk
// tags, the capability bits, and the BerryServices table the loader hands a POD at
// entry.  It is the companion of the compiler-side header shipped with the
// BerryBasiC tcc (third_party/tinycc/include/pod.h) — the BerryServices layout,
// the CAP_* values MUST stay identical to it; the services table itself now
// lives once in berry_services.h, which both headers include.
//
// PODs are produced by `tcc -pod`; see doc "The POD Executable Format".
// ===========================================================================
#include <stdint.h>
#include <stddef.h>
#include "berry_services.h"   // BerryServices, berry_arg, BERRY_ABI_VERSION

// First eight header bytes: "POD!\r\n\x1A\n".  Legible in a hex dump, and the
// \r\n\x1A\n tail trips any transfer that mangled line endings or truncated it.
#define POD_MAGIC0 0x50u   // 'P'
#define POD_MAGIC1 0x4Fu   // 'O'
#define POD_MAGIC2 0x44u   // 'D'
#define POD_MAGIC3 0x21u   // '!'

#define POD_FORMAT_VERSION 1u    // the format this loader understands
// The services ABI this loader provides is BERRY_ABI_VERSION (berry_services.h),
// the one table shared by seeds and PODs.

// Header flags (offset 14).  Bit 0 tells a program POD from an extension.
#define POD_KIND_PROGRAM   0u
#define POD_KIND_EXTENSION 1u     // bit 0 set: registers keywords, stays resident
#define POD_FLAG_SELFMOD   2u     // bit 1: wants RWX (refused unless permissive)

// Chunk relocation kinds (RLOC entry byte 4).  The loader adds the load base to
// the value already stored at the entry's offset.
#define POD_RELOC_ABS64  0
#define POD_RELOC_ABS32  1
#define POD_RELOC_REL32  2       // image-relative 32-bit

// Keyword kinds (KEYW record byte 12), matching the seed keyword kinds.
#define POD_KW_STATEMENT 0
#define POD_KW_NUMFN     1
#define POD_KW_STRFN     2

// ---------------------------------------------------------------------------
// Capability bits (header `caps`, offset 44).  The loader builds a BerryServices
// table containing only the groups the POD declared; every other slot is a
// refusal stub, so enforcement is structural rather than a per-call check.
// ---------------------------------------------------------------------------
#define POD_CAP_CONSOLE  (1ull << 0)
#define POD_CAP_VARS     (1ull << 1)
#define POD_CAP_FILES    (1ull << 2)
#define POD_CAP_DIRS     (1ull << 3)
#define POD_CAP_GRAPHICS (1ull << 4)
#define POD_CAP_SOUND    (1ull << 5)
#define POD_CAP_GPIO     (1ull << 6)
#define POD_CAP_I2C      (1ull << 7)
#define POD_CAP_TIME     (1ull << 8)
#define POD_CAP_HEAP     (1ull << 9)
#define POD_CAP_KEYWORD  (1ull << 10)
#define POD_CAP_SPAWN    (1ull << 11)
#define POD_CAP_RAWMEM   (1ull << 12)
#define POD_CAP_CORES    (1ull << 13)
#define POD_CAP_NET      (1ull << 14)
#define POD_CAP_DEBUG    (1ull << 15)   // attach the debugger (dbg_* services)

// The fixed 64-byte header, little-endian.  The loader reads the file bytewise
// into this (AArch64 is -mstrict-align), so the struct is only a field map; do
// not rely on its C layout matching the file.  Field offsets are the contract.
typedef struct pod_header {
    uint8_t  magic[8];      // 0  : "POD!\r\n\x1A\n"
    uint16_t format_ver;    // 8  : POD_FORMAT_VERSION
    uint16_t abi_ver;       // 10 : minimum BerryServices ABI required
    uint16_t chunk_count;   // 12 : number of chunks after the header
    uint16_t flags;         // 14 : POD_KIND_* / POD_FLAG_*
    uint32_t file_size;     // 16 : total file length (cross-check vs directory)
    uint32_t image_size;    // 20 : RAM to allocate for the image
    uint32_t split_off;     // 24 : offset where RX ends and RW begins (page-aligned)
    uint32_t init_size;     // 28 : bytes supplied by IMAG; the rest is zeroed BSS
    uint32_t entry_off;     // 32 : entry point offset (4-aligned, < split_off)
    uint32_t stack_need;    // 36 : stack bytes, or 0 for the default
    uint32_t heap_need;     // 40 : pod-heap bytes, or 0
    uint64_t caps;          // 44 : capability bitmask
    uint32_t build_epoch;   // 52 : seconds since 2020-01-01T00:00Z, or 0
    uint32_t payload_crc;   // 56 : CRC-32C over [64 .. start of SEAL)
    uint32_t header_crc;    // 60 : CRC-32C over [0 .. 60)
} pod_header;               // = 64 bytes

// Keyword-handler arguments arrive as berry_arg[] (see berry_services.h), shared
// with the seed argument-gathering path.

// The table handed to a POD at entry is BerryServices, defined once in
// berry_services.h and shared with seeds. The loader fills only the groups
// the POD's capabilities grant; every other slot is a refusal stub.

// A program POD's entry point.  Returns the exit status (0 = success).
typedef int (*pod_main_fn)(const BerryServices *svc,
                           int argc, const char *const *argv);
// A keyword handler.  Numeric/string handlers return the value; a statement
// handler's return is ignored.  String results are staged via the interpreter.
typedef double (*pod_kw_fn)(const BerryServices *svc,
                            const berry_arg *argv, int argc);

// ---------------------------------------------------------------------------
// Platform hooks (implemented in seed_target.c on the Pi, seed_host.c on the
// host, alongside the seed ones).  icache_sync (seed.h) is reused to make a
// freshly copied image executable before either of these is called.
// ---------------------------------------------------------------------------

// Enter a program POD.  Returns 0 and stores the exit status in *status, or -1
// if native code cannot run on this build (the host).
int pod_run_main(const void *entry, const BerryServices *svc,
                 int argc, const char *const *argv, int *status);

// Invoke a POD keyword handler.  Returns 0 and stores the numeric result in
// *out, or -1 on the host build.
int pod_run_kw(const void *entry, const BerryServices *svc,
               const berry_arg *argv, int argc, double *out);

#endif // POD_H
