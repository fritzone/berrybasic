/* tcc -seed: emit a BerryBasiC seed (.SED) from a freestanding C source.
 *
 *  A seed is the small sibling of a POD (see tccpod.c): a flat, FULLY
 *  position-independent AArch64 blob that the interpreter copies into a fixed
 *  4 KiB-aligned slot and calls.  Unlike a POD it carries no RLOC escape hatch --
 *  there is nowhere to store a load base -- so a seed with any absolute
 *  relocation is rejected outright (exactly the check the gcc seed build makes
 *  with readelf).
 *
 *  The on-disk shape (see seed/seed.h) is:
 *
 *      seed_header      16 bytes at offset 0    magic, abi, flags, entry_off
 *      seed_keyword     32 bytes at offset 16   (only for a keyword seed)
 *      <flat image>     .text/.rodata/.data laid out by tccelf
 *
 *  The header and keyword sit at the FRONT of the blob, but tcc groups sections
 *  by flags (executable first), so -- unlike the gcc+seed.ld build -- we cannot
 *  make the header a section pinned to offset 0.  Nor can we link the code at 0
 *  and prepend the header afterwards: the prefix (16 or 48) is not a multiple of
 *  4 KiB, and shifting the image by it would corrupt the low-12 address bits an
 *  ADRP+ADD pair bakes in (the slot is page-aligned, so a byte's blob offset must
 *  stay congruent mod 4096 to its link address).  Instead libtcc links the code
 *  at text_addr = 48, leaving a [0,48) gap, and this wrapper writes the header
 *  (and, for a keyword seed, the descriptor) into that gap.  Blob offset then
 *  equals link address for every byte, so the code stays position independent.
 *
 *  The source declares itself the way a POD does: a fixed entry symbol
 *  (seed_main) plus a small ".seed.desc" manifest that seed.h drops in.  See the
 *  __TINYC__ branch of SEED_EXPORT / SEED_KEYWORD in seed/seed.h.
 */

#include "tcc.h"

/* Constants mirrored from seed/seed.h (kept in sync by hand, like the POD magic
 * in tccpod.c). */
#define SEED__MAGIC       0x44454553u   /* 'S','E','E','D' little-endian */
#define SEED__HDR_KEYWORD 0x0001u       /* seed_header.flags: registers a keyword */
#define SEED__PREFIX      48            /* reserved front gap = sizeof(header)+sizeof(keyword) */

/* The ".seed.desc" manifest seed.h emits (little-endian, 28 bytes):
 *      0  u16 abi        BERRY_ABI_VERSION the source was built against
 *      2  u16 flags      0 or SEED__HDR_KEYWORD
 *      4  u16 kind       SEED_KW_* (keyword only)
 *      6  u16 min_args
 *      8  u16 max_args
 *     10  u16 reserved
 *     12  char name[16]  keyword name AS TYPED (keyword only) */
#define SEED__DESC_SIZE 28

static unsigned seed_rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static void seed_wr16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void seed_wr32(unsigned char *p, unsigned v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

/* Lay out the linked image, stamp a seed header into its reserved front, and
 * write the .SED. */
ST_FUNC int tcc_output_seed(TCCState *s1, FILE *f, const char *filename)
{
    Section *desc = NULL;
    unsigned char *image = NULL;
    addr_t image_size = 0, init_size = 0, entry_off = 0;
    unsigned abi = 0, flags = 0, kw_kind = 0, kw_min = 0, kw_max = 0;
    char kw_name[16];
    int i, ret = -1, nrloc = s1->pod_nreloc;

    memset(kw_name, 0, sizeof kw_name);

    /* --- locate the manifest section (never part of the runtime image) --- */
    for (i = 1; i < s1->nb_sections; i++)
        if (!strcmp(s1->sections[i]->name, ".seed.desc"))
            desc = s1->sections[i];

    /* --- measure the flat image (exclude the manifest) --- */
    for (i = 1; i < s1->nb_sections; i++) {
        Section *s = s1->sections[i];
        addr_t end;
        if (!(s->sh_flags & SHF_ALLOC) || s == desc || s->sh_size == 0)
            continue;
        end = s->sh_addr + s->sh_size;
        if (end > image_size)
            image_size = end;
        if (s->sh_type != SHT_NOBITS && end > init_size)
            init_size = end;              /* bytes that actually go in the file */
    }

    /* A seed cannot be relocated at load time (no base to add), so it must be
       fully position independent -- reject any harvested absolute relocation. */
    if (nrloc) {
        tcc_error_noabort("seed: image is not position independent (%d absolute "
                          "relocation(s)); avoid pointer initialisers and large "
                          "static aggregates", nrloc);
        goto done;
    }

    /* --- entry point: the fixed seed_main symbol (already past the gap) --- */
    entry_off = get_sym_addr(s1, "seed_main", 0, 0);
    if (entry_off == (addr_t)-1) {
        tcc_error_noabort("seed: no seed_main() entry point (use SEED_EXPORT or "
                          "SEED_KEYWORD from seed.h)");
        goto done;
    }
    if (entry_off & 3) {
        tcc_error_noabort("seed: seed_main is not 4-byte aligned");
        goto done;
    }
    if (entry_off < SEED__PREFIX || entry_off >= init_size) {
        tcc_error_noabort("seed: entry point outside the image");
        goto done;
    }

    /* --- read the manifest seed.h left for us --- */
    if (desc && desc->data_offset >= 4) {
        const unsigned char *p = desc->data;
        abi   = seed_rd16(p);
        flags = seed_rd16(p + 2);
        if ((flags & SEED__HDR_KEYWORD) && desc->data_offset >= SEED__DESC_SIZE) {
            kw_kind = seed_rd16(p + 4);
            kw_min  = seed_rd16(p + 6);
            kw_max  = seed_rd16(p + 8);
            memcpy(kw_name, p + 12, 16);
            kw_name[15] = 0;
        } else {
            flags &= ~SEED__HDR_KEYWORD;   /* incomplete descriptor: plain seed */
        }
    }

    /* --- flatten the loaded sections; the [0,48) gap stays zero for now --- */
    image = tcc_mallocz(init_size ? init_size : SEED__PREFIX);
    for (i = 1; i < s1->nb_sections; i++) {
        Section *s = s1->sections[i];
        if (!(s->sh_flags & SHF_ALLOC) || s == desc || s->sh_type == SHT_NOBITS)
            continue;
        if (s->sh_size)
            memcpy(image + s->sh_addr, s->data, s->sh_size);
    }

    /* --- stamp the header into the reserved front (offset 0..16) --- */
    seed_wr32(image + 0, SEED__MAGIC);
    seed_wr16(image + 4, abi);                       /* version = ABI built for */
    seed_wr16(image + 6, (unsigned)(flags & SEED__HDR_KEYWORD));
    seed_wr32(image + 8, (unsigned)entry_off);
    seed_wr32(image + 12, (unsigned)image_size);     /* total footprint incl. .bss */

    /* --- keyword descriptor at offset 16..48 (struct seed_keyword) --- */
    if (flags & SEED__HDR_KEYWORD) {
        memcpy(image + 16, kw_name, 16);             /* name[16] */
        seed_wr16(image + 32, kw_kind);              /* kind      */
        seed_wr16(image + 34, kw_min);               /* min_args  */
        seed_wr16(image + 36, kw_max);               /* max_args  */
        seed_wr16(image + 38, 0);                    /* reserved  */
        seed_wr32(image + 40, 0);                    /* pad[0]    */
        seed_wr32(image + 44, 0);                    /* pad[1]    */
    }

    if (fwrite(image, 1, (size_t)init_size, f) != (size_t)init_size) {
        tcc_error_noabort("seed: short write");
        goto done;
    }
    if (s1->verbose)
        printf("seed: %lu bytes (image %lu), entry 0x%lx, abi %u%s%s\n",
               (unsigned long)init_size, (unsigned long)image_size,
               (unsigned long)entry_off, abi,
               (flags & SEED__HDR_KEYWORD) ? ", keyword " : "",
               (flags & SEED__HDR_KEYWORD) ? kw_name : "");
    ret = 0;

done:
    tcc_free(image);
    tcc_free(s1->pod_rloc);
    s1->pod_rloc = NULL;
    s1->pod_nreloc = s1->pod_rloc_cap = 0;
    return ret;
}
