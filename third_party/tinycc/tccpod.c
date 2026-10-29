/*
 *  POD executable output for TinyCC  --  the BerryBasiC native binary format.
 *
 *  A POD ("what holds seeds") is a self-contained native program for the
 *  BerryBasiC machine: exactly one memory image with exactly one split in it
 *  (read-execute below, read-write above), wrapped by a 64-byte checksummed
 *  header and a short run of forward-only chunks:
 *
 *      MARK  provenance (plain text)     IMAG  the flat image      (mandatory)
 *      NEED  capability rationale        KEYW  keyword table (extensions)
 *      RLOC  relocations (escape hatch)  NOTE  free text
 *      SEAL  integrity, always last      (mandatory)
 *
 *  See doc "The POD Executable Format".  This file turns the statically linked,
 *  fully relocated image that tccelf.c has already laid out at base 0 into that
 *  container.  It is reached from tcc_write_elf_file() when the output format is
 *  TCC_OUTPUT_FORMAT_POD (selected by the -pod driver option, which also pins a
 *  static, freestanding link at text address 0 with a 4 KiB RX/RW split).
 *
 *  How position independence is handled.  tcc is not a PIC-only code generator,
 *  so it binds the image as if it loaded at 0.  All AArch64 control flow is
 *  PC-relative and needs no fixup; the only things bound to the base are
 *  absolute data pointers (function tables, pointer initialisers).  Every such
 *  absolute relocation is harvested here into the RLOC chunk, so the loader can
 *  add the real load base to each.  A POD with an empty RLOC is fully position
 *  independent; most carry a handful of entries.
 */

#include "tcc.h"
#include "pkt.h"

/* ---- CRC-32C (Castagnoli), the checksum every POD field is protected by.
 *  Bit-reflected poly 0x82F63B78; this is exactly what the Cortex-A72 crc32cx
 *  instruction computes, so the loader can verify at hardware speed. */
static uint32_t pod_crc32c(const void *buf, int len)
{
    const unsigned char *p = (const unsigned char *)buf;
    uint32_t crc = 0xFFFFFFFFu;
    int i, k;
    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0x82F63B78u & (uint32_t)-(int32_t)(crc & 1));
    }
    return ~crc;
}

/* ---- little-endian readers (for parsing packets we link against) ---- */
static uint16_t pod_rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t pod_rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t pod_rd64(const unsigned char *p) {
    return (uint64_t)pod_rd32(p) | ((uint64_t)pod_rd32(p + 4) << 32);
}

/* ---- a small growable little-endian byte buffer ---- */
typedef struct {
    unsigned char *d;
    int len, cap;
} PodBuf;

static void pb_room(PodBuf *b, int n)
{
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 128;
        b->d = tcc_realloc(b->d, b->cap);
    }
}
static void pb_raw(PodBuf *b, const void *p, int n)
{
    pb_room(b, n);
    memcpy(b->d + b->len, p, n);
    b->len += n;
}
static void pb_u8(PodBuf *b, unsigned v)  { unsigned char c = (unsigned char)v; pb_raw(b, &c, 1); }
static void pb_u16(PodBuf *b, unsigned v) { pb_u8(b, v); pb_u8(b, v >> 8); }
static void pb_u32(PodBuf *b, uint32_t v) { pb_u16(b, v); pb_u16(b, v >> 16); }
static void pb_u64(PodBuf *b, uint64_t v) { pb_u32(b, (uint32_t)v); pb_u32(b, (uint32_t)(v >> 32)); }
static void pb_pad4(PodBuf *b)            { while (b->len & 3) pb_u8(b, 0); }
static void pod_poke32(PodBuf *b, int off, uint32_t v)
{
    b->d[off]   = (unsigned char)v;
    b->d[off+1] = (unsigned char)(v >> 8);
    b->d[off+2] = (unsigned char)(v >> 16);
    b->d[off+3] = (unsigned char)(v >> 24);
}

/* Emit one chunk: 12-byte prelude {tag, size, crc-of-payload} + payload + pad. */
static void pod_chunk(PodBuf *b, const char *tag, const void *payload, int len)
{
    pb_raw(b, tag, 4);
    pb_u32(b, (uint32_t)len);
    pb_u32(b, pod_crc32c(payload, len));
    pb_raw(b, payload, len);
    pb_pad4(b);
}

/* Which RLOC kind an ELF relocation type maps to, or -1 if it needs no fixup
 * (PC-relative types are position independent and are simply skipped). */
static int pod_reloc_kind(int type)
{
    switch (type) {
#if defined TCC_TARGET_ARM64
    case R_AARCH64_ABS64: return 0;
    case R_AARCH64_ABS32: return 1;
#elif defined TCC_TARGET_X86_64
    case R_X86_64_64:  return 0;
    case R_X86_64_32:
    case R_X86_64_32S: return 1;
#elif defined TCC_TARGET_ARM
    case R_ARM_ABS32:  return 1;
#elif defined TCC_TARGET_RISCV64
    case R_RISCV_64:   return 0;
    case R_RISCV_32:   return 1;
#elif defined TCC_TARGET_I386
    case R_386_32:     return 1;
#endif
    default: return -1;
    }
}

/* ---- the parsed .pod.desc manifest ---- */
#define POD__T_NAME  1
#define POD__T_VERS  2
#define POD__T_AUTH  3
#define POD__T_DESC  4
#define POD__T_LICE  5
#define POD__T_CAPS  6
#define POD__T_NEED  7
#define POD__T_FLAGS 8
#define POD__T_ABI   9
#define POD__T_KEYW 10

typedef struct {
    const char *name, *vers, *auth, *desc, *lice; /* MARK text, or NULL */
    uint64_t caps;
    uint16_t hdr_flags;
    uint16_t abi;
    PodBuf   need;   /* accumulated "CAP=reason\0" lines               */
    PodBuf   keyw;   /* accumulated raw 24-byte KEYW records           */
    int      nkeyw;
} PodManifest;

/* Read a little-endian u16/u32/u64 out of the manifest byte stream. */
static unsigned pod_get16(const unsigned char *p) { return p[0] | (p[1] << 8); }

/* Walk the packed {u16 tag; u16 len; bytes} records emitted by pod.h. */
static int pod_parse_desc(TCCState *s1, Section *desc, PodManifest *m)
{
    const unsigned char *p, *end;
    if (!desc)
        return 0;
    p = desc->data;
    end = p + desc->data_offset;
    while (p + 4 <= end) {
        unsigned tag = pod_get16(p);
        unsigned len = pod_get16(p + 2);
        const unsigned char *pl = p + 4;
        if (pl + len > end)
            break;
        switch (tag) {
        case POD__T_NAME: m->name = (const char *)pl; break;
        case POD__T_VERS: m->vers = (const char *)pl; break;
        case POD__T_AUTH: m->auth = (const char *)pl; break;
        case POD__T_DESC: m->desc = (const char *)pl; break;
        case POD__T_LICE: m->lice = (const char *)pl; break;
        case POD__T_ABI:  if (len >= 2) m->abi = (uint16_t)pod_get16(pl); break;
        case POD__T_FLAGS:if (len >= 2) m->hdr_flags |= (uint16_t)pod_get16(pl); break;
        case POD__T_CAPS:
            if (len >= 8) {
                uint64_t c = 0; int i;
                for (i = 0; i < 8; i++) c |= (uint64_t)pl[i] << (8 * i);
                m->caps |= c;
            }
            break;
        case POD__T_NEED: {
            int sl = (int)strlen((const char *)pl); /* ignore any trailing pad */
            pb_raw(&m->need, pl, sl + 1);            /* keep the NUL separator  */
            break;
        }
        case POD__T_KEYW: {
            /* record: name[12], kind, mn, mx, flags, sym[20] (packed, 36B) */
            char rec[24];
            addr_t off;
            const char *sym;
            if (len < 36) break;
            memset(rec, 0, sizeof rec);
            memcpy(rec, pl, 12);                 /* name[12] */
            rec[12] = pl[12];                    /* kind      */
            rec[13] = pl[13];                    /* min_args  */
            rec[14] = pl[14];                    /* max_args  */
            rec[15] = pl[15];                    /* flags     */
            sym = (const char *)pl + 16;         /* handler symbol name */
            off = get_sym_addr(s1, sym, 0, 0);
            if (off == (addr_t)-1)
                return tcc_error_noabort("POD keyword handler '%s' not defined", sym);
            rec[16] = (unsigned char)off;
            rec[17] = (unsigned char)(off >> 8);
            rec[18] = (unsigned char)(off >> 16);
            rec[19] = (unsigned char)(off >> 24);
            /* rec[20..23] reserved = 0 */
            pb_raw(&m->keyw, rec, 24);
            m->nkeyw++;
            m->hdr_flags |= 1;                   /* POD_KIND = extension */
            break;
        }
        default: break;                          /* unknown: preserved by tools, ignored here */
        }
        p = pl + len;
    }
    return 0;
}

/* basename without directory or extension, for a default MARK name= */
static void pod_default_name(const char *filename, char *out, int outsz)
{
    const char *base = filename, *q, *dot;
    int n;
    for (q = filename; *q; q++)
        if (*q == '/' || *q == '\\')
            base = q + 1;
    dot = strrchr(base, '.');
    n = dot ? (int)(dot - base) : (int)strlen(base);
    if (n > outsz - 1) n = outsz - 1;
    memcpy(out, base, n);
    out[n] = 0;
}

static void pod_mark_kv(PodBuf *b, const char *key, const char *val)
{
    if (!val) return;
    pb_raw(b, key, (int)strlen(key));
    pb_u8(b, '=');
    pb_raw(b, val, (int)strlen(val) + 1);        /* value + NUL */
}

/* Append one RLOC entry (u32 offset, u8 kind, 3 pad) to the harvested list. */
static void pod_add_rloc(TCCState *s1, uint32_t off, int kind)
{
    unsigned char *e;
    if (s1->pod_nreloc * 8 + 8 > s1->pod_rloc_cap) {
        s1->pod_rloc_cap = s1->pod_rloc_cap * 2 + 256;
        s1->pod_rloc = tcc_realloc(s1->pod_rloc, s1->pod_rloc_cap);
    }
    e = s1->pod_rloc + s1->pod_nreloc * 8;
    e[0] = (unsigned char)off;       e[1] = (unsigned char)(off >> 8);
    e[2] = (unsigned char)(off >> 16); e[3] = (unsigned char)(off >> 24);
    e[4] = (unsigned char)kind;      e[5] = e[6] = e[7] = 0;
    s1->pod_nreloc++;
}

/* Gather every absolute fixup the loader must apply after choosing a load base.
 * Called from elf_output_file() after the GOT is filled but before the .rela
 * sections are reordered away.  Two sources:
 *   1. the .rela sections: entries whose type is an absolute (ABS64/ABS32) fixup
 *      of a loaded section (PC-relative entries need no fixup and are skipped);
 *   2. the filled GOT: tcc binds each used slot to a symbol's base-0 address, so
 *      every non-zero slot is itself an absolute pointer to relocate. */
ST_FUNC void pod_collect_relocs(TCCState *s1)
{
    int i, gotkind = (PTR_SIZE == 8) ? 0 : 1;
    Section *desc = NULL;

    for (i = 1; i < s1->nb_sections; i++)
        if (!strcmp(s1->sections[i]->name, ".pod.desc"))
            { desc = s1->sections[i]; break; }

    for (i = 1; i < s1->nb_sections; i++) {
        Section *sr = s1->sections[i], *s;
        ElfW_Rel *rel;
        if (sr->sh_type != SHT_RELX)
            continue;
        s = s1->sections[sr->sh_info];
        if (!(s->sh_flags & SHF_ALLOC) || s == desc)
            continue;
        for_each_elem(sr, 0, rel, ElfW_Rel) {
            int kind = pod_reloc_kind(ELFW(R_TYPE)(rel->r_info));
            if (kind < 0)
                continue;                         /* PC-relative: position independent */
            pod_add_rloc(s1, (uint32_t)(s->sh_addr + rel->r_offset), kind);
        }
    }

    if (s1->got && (s1->got->sh_flags & SHF_ALLOC)) {
        Section *g = s1->got;
        unsigned long o;
        for (o = 0; o + PTR_SIZE <= g->data_offset; o += PTR_SIZE) {
            uint64_t v = 0;
            int k;
            for (k = 0; k < PTR_SIZE; k++)
                v |= (uint64_t)g->data[o + k] << (8 * k);
            if (v != 0)                           /* a bound slot: an image pointer */
                pod_add_rloc(s1, (uint32_t)(g->sh_addr + o), gotkind);
        }
    }
}

/* ==========================================================================
 * Consuming a seed packet (.PKT): a static library for PODs.
 *
 * Classic archive link-closure: pull whole members to satisfy outstanding
 * undefined references, repeating until nothing new is pulled, and OR each
 * pulled member's declared capabilities into the POD's (so a library cannot
 * smuggle a capability past the header that PODINFO will show).  See pkt.h.
 * ========================================================================== */
ST_FUNC int tcc_load_packet(TCCState *s1, int fd)
{
    unsigned char hdr[PKT_HDR_SIZE], *d = NULL;
    int i, ret = -1, fsize, mcnt, scnt, npool;
    char *loaded = NULL;

    lseek(fd, 0, SEEK_SET);
    if (full_read(fd, hdr, PKT_HDR_SIZE) != PKT_HDR_SIZE
        || hdr[0] != PKT_MAGIC0 || hdr[1] != PKT_MAGIC1
        || hdr[2] != PKT_MAGIC2 || hdr[3] != PKT_MAGIC3)
        return tcc_error_noabort("Not a packet file");
    if (pod_crc32c(hdr, PKT_H_HEADER_CRC) != pod_rd32(hdr + PKT_H_HEADER_CRC))
        return tcc_error_noabort("Packet is damaged");
    if (pod_rd16(hdr + PKT_H_FORMAT_VER) > PKT_FORMAT_VERSION)
        return tcc_error_noabort("Packet needs a newer system");

    fsize = (int)pod_rd32(hdr + PKT_H_FILESIZE);
    mcnt  = (int)pod_rd32(hdr + PKT_H_MEMBER_CNT);
    scnt  = (int)pod_rd32(hdr + PKT_H_SYMBOL_CNT);
    npool = (int)pod_rd32(hdr + PKT_H_NAMEPOOL);
    (void)npool;

    d = load_data(fd, 0, fsize);           /* the whole packet, for the tables */
    loaded = tcc_mallocz(mcnt ? mcnt : 1);

    /* verify each member's bytes before pulling anything */
    for (i = 0; i < mcnt; i++) {
        unsigned char *m = d + PKT_HDR_SIZE + i * PKT_MEMBER_SIZE;
        uint32_t off = pod_rd32(m + PKT_M_DATA_OFF), sz = pod_rd32(m + PKT_M_DATA_SIZE);
        if (off + sz > (uint32_t)fsize || pod_crc32c(d + off, sz) != pod_rd32(m + PKT_M_CRC)) {
            tcc_error_noabort("Packet is damaged"); goto done;
        }
    }

    /* closure: pull members whose symbols resolve an outstanding undefined */
    for (;;) {
        int bound = 0;
        for (i = 0; i < scnt; i++) {
            unsigned char *sr = d + PKT_HDR_SIZE + mcnt * PKT_MEMBER_SIZE + i * PKT_SYMBOL_SIZE;
            const char *name = (const char *)d + pod_rd32(sr + PKT_S_NAME_OFF);
            int member = (int)pod_rd32(sr + PKT_S_MEMBER);
            int si = find_elf_sym(symtab_section, name);
            ElfW(Sym) *sym;
            if (member < 0 || member >= mcnt || loaded[member]) continue;
            if (!si) continue;
            sym = &((ElfW(Sym) *)symtab_section->data)[si];
            if (sym->st_shndx != SHN_UNDEF) continue;   /* already defined */

            unsigned char *m = d + PKT_HDR_SIZE + member * PKT_MEMBER_SIZE;
            uint32_t off = pod_rd32(m + PKT_M_DATA_OFF);
            if (s1->verbose == 2)
                printf("   -> %s (%s)\n", (const char *)d + pod_rd32(m + PKT_M_NAME_OFF), name);
            if (tcc_load_object_file(s1, fd, off) < 0) goto done;
            s1->pod_lib_caps |= pod_rd64(m + PKT_M_CAPS);   /* capabilities compose */
            loaded[member] = 1;
            bound = 1;
        }
        if (!bound) break;
    }
    ret = 0;
done:
    tcc_free(loaded);
    tcc_free(d);
    return ret;
}

/* ==========================================================================
 * The writer proper.
 * ========================================================================== */
ST_FUNC int tcc_output_pod(TCCState *s1, FILE *f, const char *filename)
{
    PodManifest m;
    PodBuf file, mark;
    Section *desc = NULL;
    unsigned char *image = NULL;
    addr_t split_off = 0, image_size = 0, init_size = 0, entry_off = 0;
    int have_split = 0, nrloc = 0, chunk_count = 0, seal_pos, i, ret = -1;

    memset(&m, 0, sizeof m);
    memset(&file, 0, sizeof file);
    memset(&mark, 0, sizeof mark);
    m.abi = 1;
    nrloc = s1->pod_nreloc;                       /* harvested in pod_collect_relocs */

    /* --- locate the manifest section (never part of the runtime image) --- */
    for (i = 1; i < s1->nb_sections; i++)
        if (!strcmp(s1->sections[i]->name, ".pod.desc"))
            desc = s1->sections[i];
    if (pod_parse_desc(s1, desc, &m) < 0)
        goto done;

    /* --- measure the image: RX below split_off, RW above, BSS zero-filled --- */
    for (i = 1; i < s1->nb_sections; i++) {
        Section *s = s1->sections[i];
        addr_t sopdend;
        if (!(s->sh_flags & SHF_ALLOC))
            continue;
        if (s == desc)
            continue;                             /* manifest stays out of the image */
        if (s->sh_size == 0)
            continue;                             /* empty section: no bytes, no split */
        sopdend = s->sh_addr + s->sh_size;
        if (sopdend > image_size)
            image_size = sopdend;
        if (s->sh_type != SHT_NOBITS && sopdend > init_size)
            init_size = sopdend;
        if (s->sh_flags & SHF_WRITE) {
            if (!have_split || s->sh_addr < split_off) {
                split_off = s->sh_addr;
                have_split = 1;
            }
        }
    }
    /* No writable data at all: the whole image is RX; park the (empty) RW
       region on the next page so split_off stays page-aligned and < image. */
    if (!have_split)
        split_off = (init_size + 0xFFF) & ~(addr_t)0xFFF;
    if (image_size < split_off)
        image_size = split_off;

    if (split_off & 0xFFF)
        { tcc_error_noabort("POD image: RX/RW split 0x%lx is not page-aligned",
                            (unsigned long)split_off); goto done; }

    /* --- entry point --- */
    entry_off = get_sym_addr(s1, "pod_main", 0, 0);
    if (entry_off == (addr_t)-1) {
        if (!(m.hdr_flags & 1)) {  /* a program POD must have pod_main */
            tcc_error_noabort("POD: no pod_main (and no keywords); nothing to enter");
            goto done;
        }
        entry_off = 0;             /* extension POD: entry is per-keyword */
    } else if (entry_off & 3) {
        tcc_error_noabort("POD: pod_main is not 4-byte aligned"); goto done;
    } else if (entry_off >= split_off) {
        tcc_error_noabort("POD: entry point lies in writable memory"); goto done;
    }

    /* --- flatten the loaded sections into the IMAG bytes --- */
    image = tcc_mallocz(init_size ? init_size : 1);
    for (i = 1; i < s1->nb_sections; i++) {
        Section *s = s1->sections[i];
        if (!(s->sh_flags & SHF_ALLOC) || s == desc)
            continue;
        if (s->sh_type == SHT_NOBITS)
            continue;
        if (s->sh_size)
            memcpy(image + s->sh_addr, s->data, s->sh_size);
    }

    /* Absolute relocations were harvested in pod_collect_relocs (s1->pod_rloc),
       while the .rela sections and filled GOT were still available. */
    if (nrloc && (m.hdr_flags & 1)) {
        tcc_error_noabort("POD: an extension POD must be position independent "
                          "(it has %d absolute relocation(s)); avoid pointer "
                          "initialisers in keyword code", nrloc);
        goto done;
    }

    /* --- MARK: provenance, plain text key=value records --- */
    {
        char namebuf[64], toolbuf[80], datebuf[48];
        time_t now = time(NULL);
        struct tm *g = gmtime(&now);
        if (!m.name) { pod_default_name(filename, namebuf, sizeof namebuf); }
        pod_mark_kv(&mark, "name", m.name ? m.name : namebuf);
        pod_mark_kv(&mark, "vers", m.vers);
        pod_mark_kv(&mark, "auth", m.auth);
        snprintf(toolbuf, sizeof toolbuf, "tcc-%s-berry", TCC_VERSION);
        pod_mark_kv(&mark, "tool", toolbuf);
        if (g) {
            snprintf(datebuf, sizeof datebuf, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                     g->tm_year + 1900, g->tm_mon + 1, g->tm_mday,
                     g->tm_hour, g->tm_min, g->tm_sec);
            pod_mark_kv(&mark, "date", datebuf);
        }
        pod_mark_kv(&mark, "desc", m.desc);
        pod_mark_kv(&mark, "lice", m.lice);
    }

    /* Capabilities compose: union in whatever the linked-in packet members
       declared (tcc_load_packet), so the header reflects everything actually
       linked, not just what the main source declared. */
    m.caps |= s1->pod_lib_caps;

    /* ===================== assemble the file ===================== */

    /* Fixed 64-byte header, filled in now, back-patched (sizes + CRCs) later. */
    pb_raw(&file, "\x50\x4F\x44\x21\x0D\x0A\x1A\x0A", 8); /* magic POD!\r\n\x1A\n */
    pb_u16(&file, 1);                 /* off 8  format_ver                     */
    pb_u16(&file, m.abi);             /* off 10 abi_ver                        */
    pb_u16(&file, 0);                 /* off 12 chunk_count (patched)          */
    pb_u16(&file, m.hdr_flags);       /* off 14 flags                          */
    pb_u32(&file, 0);                 /* off 16 file_size (patched)            */
    pb_u32(&file, (uint32_t)image_size);/* off 20 image_size                   */
    pb_u32(&file, (uint32_t)split_off); /* off 24 split_off                    */
    pb_u32(&file, (uint32_t)init_size); /* off 28 init_size                    */
    pb_u32(&file, (uint32_t)entry_off); /* off 32 entry_off                    */
    pb_u32(&file, 0);                 /* off 36 stack_need (default)           */
    pb_u32(&file, 0);                 /* off 40 heap_need                      */
    pb_u64(&file, m.caps);            /* off 44 caps                           */
    {   /* off 52 build_epoch: seconds since 2020-01-01T00:00Z */
        time_t now = time(NULL);
        long e = (long)now - 1577836800L;         /* 2020-01-01 in Unix time */
        pb_u32(&file, e > 0 ? (uint32_t)e : 0);
    }
    pb_u32(&file, 0);                 /* off 56 payload_crc (patched)          */
    pb_u32(&file, 0);                 /* off 60 header_crc  (patched)          */
    /* header is now 64 bytes */

    /* MARK (mandatory for distribution: we always have tool+date) */
    pod_chunk(&file, "MARK", mark.d, mark.len); chunk_count++;

    /* NEED (only if any capability rationale was declared) */
    if (m.need.len) { pod_chunk(&file, "NEED", m.need.d, m.need.len); chunk_count++; }

    /* IMAG (mandatory): exactly init_size bytes */
    pod_chunk(&file, "IMAG", image, (int)init_size); chunk_count++;

    /* KEYW (extension PODs): count + 24-byte records */
    if (m.nkeyw) {
        PodBuf kw; memset(&kw, 0, sizeof kw);
        pb_u32(&kw, (uint32_t)m.nkeyw);
        pb_raw(&kw, m.keyw.d, m.keyw.len);
        pod_chunk(&file, "KEYW", kw.d, kw.len); chunk_count++;
        tcc_free(kw.d);
    }

    /* RLOC (only if the image is not fully position independent): a count
       followed by that many 8-byte {offset, kind, rsv[3]} entries. */
    if (nrloc) {
        PodBuf rc; memset(&rc, 0, sizeof rc);
        pb_u32(&rc, (uint32_t)nrloc);
        pb_raw(&rc, s1->pod_rloc, nrloc * 8);
        pod_chunk(&file, "RLOC", rc.d, rc.len); chunk_count++;
        tcc_free(rc.d);
    }

    /* NOTE: a friendly line for whoever opens the file */
    {
        static const char note[] =
            "Built by tcc -pod for BerryBasiC. Every byte is checksummed; "
            "the MARK chunk above says where this came from.";
        pod_chunk(&file, "NOTE", note, (int)sizeof note); chunk_count++;
    }

    /* SEAL is always the last chunk and always the same length: a 12-byte
       prelude + a 40-byte payload = 52 bytes, already 4-aligned.  Because that
       size is known in advance, the header (which carries file_size and its own
       CRC) can be finalised BEFORE the seal is written, which keeps every
       checksum's input frozen by the time the next one is computed. */
    seal_pos = file.len;                          /* where the SEAL prelude begins */
    chunk_count++;                                /* count SEAL itself             */

    file.d[12] = (unsigned char)chunk_count;      /* chunk_count (off 12, u16)     */
    file.d[13] = (unsigned char)(chunk_count >> 8);
    pod_poke32(&file, 16, (uint32_t)(seal_pos + 52)); /* file_size (off 16)        */
    /* payload_crc: bytes [64 .. start of SEAL) */
    pod_poke32(&file, 56, pod_crc32c(file.d + 64, seal_pos - 64));
    /* header_crc: bytes [0 .. 60), with the two fields above already in place */
    pod_poke32(&file, 60, pod_crc32c(file.d, 60));

    /* SEAL payload: method 0 (CRC only); whole_crc over everything before the
       seal ([0, seal_pos)), so it never has to checksum itself; digest zeroed. */
    {
        unsigned char pl[40];
        uint32_t whole = pod_crc32c(file.d, seal_pos);
        memset(pl, 0, sizeof pl);
        pl[0] = 0;                                /* method 0: CRC only  */
        pl[4] = (unsigned char)whole;             /* whole_crc (LE)      */
        pl[5] = (unsigned char)(whole >> 8);
        pl[6] = (unsigned char)(whole >> 16);
        pl[7] = (unsigned char)(whole >> 24);
        pod_chunk(&file, "SEAL", pl, (int)sizeof pl);
    }

    if (file.len != seal_pos + 52) {
        tcc_error_noabort("POD: internal size mismatch (%d vs %d)",
                          file.len, seal_pos + 52);
        goto done;
    }
    if (fwrite(file.d, 1, file.len, f) != (size_t)file.len) {
        tcc_error_noabort("POD: short write"); goto done;
    }
    if (s1->verbose)
        printf("POD: %d bytes, image %lu (init %lu, split 0x%lx), %d reloc(s), "
               "entry 0x%lx, caps 0x%llx\n",
               file.len, (unsigned long)image_size, (unsigned long)init_size,
               (unsigned long)split_off, nrloc, (unsigned long)entry_off,
               (unsigned long long)m.caps);
    ret = 0;

done:
    tcc_free(image);
    tcc_free(file.d);
    tcc_free(mark.d);
    tcc_free(m.need.d);
    tcc_free(m.keyw.d);
    tcc_free(s1->pod_rloc);
    s1->pod_rloc = NULL;
    s1->pod_nreloc = s1->pod_rloc_cap = 0;
    return ret;
}
