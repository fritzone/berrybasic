/*
 *  tcc -pkt : build (and list) a BerryBasiC seed packet (.PKT), a static library
 *  for PODs.  See pkt.h and doc "Seed Packets".
 *
 *      tcc -pkt MATH.PKT sqrt.o trig.o fixed.o     # create
 *      tcc -pkt list MATH.PKT                       # inspect
 *
 *  Creating a packet derives the symbol table by scanning each object's own ELF
 *  symbol information (the same the linker consumes), and each member's declared
 *  capabilities from its .pod.desc section, so a member cannot smuggle a
 *  capability past the POD header that will be computed from it.
 */
#include "tcc.h"
#include "pkt.h"
#include <stdarg.h>

/* tcc_error_noabort is a state-carrying macro (needs an s1 in scope); the tool
 * code, like tcc -ar, reports to stderr directly. */
static int pkt_error(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return -1;
}

/* CRC-32C (Castagnoli), as everywhere in the POD/PKT world. */
static uint32_t pkt_crc32c(const void *buf, int len)
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

/* --- a small growable byte buffer --- */
typedef struct { unsigned char *d; int len, cap; } PktBuf;
static void pkb_room(PktBuf *b, int n) {
    if (b->len + n > b->cap) { b->cap = (b->len + n) * 2 + 256; b->d = tcc_realloc(b->d, b->cap); }
}
static void pkb_raw(PktBuf *b, const void *p, int n) { pkb_room(b, n); memcpy(b->d + b->len, p, n); b->len += n; }
static int  pkb_str(PktBuf *b, const char *s) {           /* append a C string, return its offset */
    int off = b->len; pkb_raw(b, s, (int)strlen(s) + 1); return off;
}
static void pkt_wr16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void pkt_wr32(unsigned char *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void pkt_wr64(unsigned char *p, uint64_t v) { pkt_wr32(p, (uint32_t)v); pkt_wr32(p+4, (uint32_t)(v>>32)); }
static uint16_t pkt_rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static uint32_t pkt_rd32(const unsigned char *p) { return (uint32_t)p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint64_t pkt_rd64(const unsigned char *p) { return (uint64_t)pkt_rd32(p) | ((uint64_t)pkt_rd32(p+4)<<32); }

/* --- a parsed object member --- */
typedef struct { char *bytes; int size; uint32_t crc; uint64_t caps; int name_off; char *base; } PktMember;
typedef struct { char *name; int name_off, member, kind; } PktSym;

/* Read the whole file. Returns malloc'd bytes and sets *size, or 0. */
static char *pkt_read_file(const char *path, int *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = tcc_malloc((int)n + 1);
    if (fread(b, 1, n, f) != (size_t)n) { fclose(f); tcc_free(b); return 0; }
    fclose(f); *size = (int)n; return b;
}

/* OR in the CAP_* bits declared in an object's .pod.desc (records are
 * {u16 tag; u16 len; payload}; tag 6 = an 8-byte capability mask). */
static uint64_t pkt_scan_caps(const unsigned char *desc, int dlen) {
    uint64_t caps = 0;
    int p = 0;
    while (p + 4 <= dlen) {
        unsigned tag = pkt_rd16(desc + p), len = pkt_rd16(desc + p + 2);
        if (p + 4 + (int)len > dlen) break;
        if (tag == 6 /* POD caps record */ && len >= 8) caps |= pkt_rd64(desc + p + 4);
        p += 4 + len;
    }
    return caps;
}

/* Parse one ELF object: fill *m, and append its defined global/weak symbols to
 * *syms (as (name, member index) pairs). Returns 0 on success. */
static int pkt_parse_object(const char *path, int midx, PktMember *m,
                            PktSym **syms, int *nsym, int *symcap) {
    int size;
    char *buf = pkt_read_file(path, &size);
    if (!buf) return pkt_error("tcc: pkt: cannot read %s", path);
    ElfW(Ehdr) *eh = (ElfW(Ehdr) *)buf;
    if (size < (int)sizeof *eh || eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E'
        || eh->e_ident[4] != ELFCLASSW || eh->e_type != ET_REL) {
        tcc_free(buf); return pkt_error("tcc: pkt: %s is not a relocatable object", path);
    }
    /* base name */
    const char *base = path, *q;
    for (q = path; *q; q++) if (*q == '/' || *q == '\\') base = q + 1;

    ElfW(Shdr) *sh = (ElfW(Shdr) *)(buf + eh->e_shoff);
    const char *shstr = buf + sh[eh->e_shstrndx].sh_offset;
    const char *symtab = 0, *strtab = 0, *desc = 0;
    int symsz = 0, desclen = 0;
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) { symtab = buf + sh[i].sh_offset; symsz = sh[i].sh_size; }
        else if (sh[i].sh_type == SHT_STRTAB && !strcmp(shstr + sh[i].sh_name, ".strtab"))
            strtab = buf + sh[i].sh_offset;
        else if (!strcmp(shstr + sh[i].sh_name, ".pod.desc")) { desc = buf + sh[i].sh_offset; desclen = sh[i].sh_size; }
    }

    m->bytes = buf; m->size = size; m->base = tcc_strdup(base);
    m->crc = pkt_crc32c(buf, size);
    m->caps = desc ? pkt_scan_caps((const unsigned char *)desc, desclen) : 0;

    if (symtab && strtab) {
        int n = symsz / (int)sizeof(ElfW(Sym));
        for (int i = 1; i < n; i++) {
            ElfW(Sym) *s = (ElfW(Sym) *)(symtab + i * sizeof(ElfW(Sym)));
            int bind = ELFW(ST_BIND)(s->st_info), type = ELFW(ST_TYPE)(s->st_info);
            if (s->st_shndx == SHN_UNDEF) continue;           /* only definitions */
            if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
            const char *nm = strtab + s->st_name;
            if (!nm[0]) continue;
            if (*nsym >= *symcap) { *symcap = *symcap * 2 + 64; *syms = tcc_realloc(*syms, *symcap * sizeof(PktSym)); }
            PktSym *ps = &(*syms)[(*nsym)++];
            ps->name = tcc_strdup(nm);
            ps->member = midx;
            ps->kind = (type == STT_FUNC) ? PKT_SYM_FUNC : PKT_SYM_DATA;
            ps->name_off = 0;
        }
    }
    return 0;
}

static int pkt_symcmp(const void *a, const void *b) {
    return strcmp(((const PktSym *)a)->name, ((const PktSym *)b)->name);
}

/* --- create OUT from the given object files --- */
static int pkt_create(const char *out, int nfiles, char **files) {
    PktMember *mem = tcc_mallocz(nfiles * sizeof(PktMember));
    PktSym *syms = 0; int nsym = 0, symcap = 0, ret = -1;

    for (int i = 0; i < nfiles; i++)
        if (pkt_parse_object(files[i], i, &mem[i], &syms, &nsym, &symcap) < 0)
            goto done;

    /* duplicate-symbol check (a link-time error, but cheap to catch on create) */
    qsort(syms, nsym, sizeof(PktSym), pkt_symcmp);
    for (int i = 1; i < nsym; i++)
        if (!strcmp(syms[i].name, syms[i-1].name)) {
            pkt_error("tcc: pkt: duplicate definition of %s", syms[i].name); goto done;
        }

    /* name pool: member base names, then symbol names */
    PktBuf names; memset(&names, 0, sizeof names);
    for (int i = 0; i < nfiles; i++) mem[i].name_off = pkb_str(&names, mem[i].base);
    for (int i = 0; i < nsym; i++)    syms[i].name_off = pkb_str(&names, syms[i].name);

    /* layout */
    int namepool_off = PKT_HDR_SIZE + nfiles * PKT_MEMBER_SIZE + nsym * PKT_SYMBOL_SIZE;
    int data_off = namepool_off + names.len;
    for (int i = 0; i < nfiles; i++) { mem[i].name_off += namepool_off; }  /* offsets are file-absolute */
    for (int i = 0; i < nsym; i++)    { syms[i].name_off += namepool_off; }

    PktBuf f; memset(&f, 0, sizeof f);
    /* header (patched below) */
    unsigned char hdr[PKT_HDR_SIZE]; memset(hdr, 0, sizeof hdr);
    hdr[0]=PKT_MAGIC0; hdr[1]=PKT_MAGIC1; hdr[2]=PKT_MAGIC2; hdr[3]=PKT_MAGIC3;
    hdr[4]=0x0D; hdr[5]=0x0A; hdr[6]=0x1A; hdr[7]=0x0A;
    pkt_wr16(hdr + PKT_H_FORMAT_VER, PKT_FORMAT_VERSION);
    pkt_wr32(hdr + PKT_H_MEMBER_CNT, nfiles);
    pkt_wr32(hdr + PKT_H_SYMBOL_CNT, nsym);
    pkt_wr32(hdr + PKT_H_NAMEPOOL,   namepool_off);
    pkb_raw(&f, hdr, PKT_HDR_SIZE);

    /* member table */
    int running = data_off;
    for (int i = 0; i < nfiles; i++) {
        unsigned char r[PKT_MEMBER_SIZE]; memset(r, 0, sizeof r);
        pkt_wr32(r + PKT_M_NAME_OFF, mem[i].name_off);
        pkt_wr32(r + PKT_M_DATA_OFF, running);
        pkt_wr32(r + PKT_M_DATA_SIZE, mem[i].size);
        pkt_wr32(r + PKT_M_CRC, mem[i].crc);
        pkt_wr64(r + PKT_M_CAPS, mem[i].caps);
        pkb_raw(&f, r, PKT_MEMBER_SIZE);
        running += mem[i].size;
    }
    /* symbol table (already sorted) */
    for (int i = 0; i < nsym; i++) {
        unsigned char r[PKT_SYMBOL_SIZE]; memset(r, 0, sizeof r);
        pkt_wr32(r + PKT_S_NAME_OFF, syms[i].name_off);
        pkt_wr32(r + PKT_S_MEMBER, syms[i].member);
        r[PKT_S_KIND] = (unsigned char)syms[i].kind;
        pkb_raw(&f, r, PKT_SYMBOL_SIZE);
    }
    /* name pool + member data */
    pkb_raw(&f, names.d, names.len);
    for (int i = 0; i < nfiles; i++) pkb_raw(&f, mem[i].bytes, mem[i].size);

    /* file_size + header_crc */
    pkt_wr32(f.d + PKT_H_FILESIZE, f.len);
    pkt_wr32(f.d + PKT_H_HEADER_CRC, pkt_crc32c(f.d, PKT_H_HEADER_CRC));

    {
        FILE *o = fopen(out, "wb");
        if (!o) { pkt_error("tcc: pkt: cannot write %s", out); tcc_free(f.d); tcc_free(names.d); goto done; }
        fwrite(f.d, 1, f.len, o); fclose(o);
    }
    if (nsym)
        printf("packet %s: %d member(s), %d symbol(s), %d bytes\n", out, nfiles, nsym, f.len);
    tcc_free(f.d); tcc_free(names.d);
    ret = 0;
done:
    for (int i = 0; i < nfiles; i++) { tcc_free(mem[i].bytes); tcc_free(mem[i].base); }
    for (int i = 0; i < nsym; i++) tcc_free(syms[i].name);
    tcc_free(mem); tcc_free(syms);
    return ret;
}

/* --- list a packet's members, sizes, capabilities and exported symbols --- */
static const struct { uint64_t bit; const char *name; } pkt_capnames[] = {
    {1ull<<0,"CONSOLE"},{1ull<<1,"VARS"},{1ull<<2,"FILES"},{1ull<<3,"DIRS"},
    {1ull<<4,"GRAPHICS"},{1ull<<5,"SOUND"},{1ull<<6,"GPIO"},{1ull<<7,"I2C"},
    {1ull<<8,"TIME"},{1ull<<9,"HEAP"},{1ull<<10,"KEYWORD"},{1ull<<12,"RAWMEM"},
};
static int pkt_list(const char *path) {
    int size; char *b = pkt_read_file(path, &size);
    if (!b) return pkt_error("tcc: %s: No such packet", path);
    unsigned char *d = (unsigned char *)b;
    if (size < PKT_HDR_SIZE || d[0]!=PKT_MAGIC0 || d[1]!=PKT_MAGIC1 || d[2]!=PKT_MAGIC2 || d[3]!=PKT_MAGIC3) {
        tcc_free(b); return pkt_error("tcc: %s: Not a packet file", path);
    }
    if (pkt_crc32c(d, PKT_H_HEADER_CRC) != pkt_rd32(d + PKT_H_HEADER_CRC)) {
        tcc_free(b); return pkt_error("tcc: %s: Packet is damaged", path);
    }
    int nm = pkt_rd32(d + PKT_H_MEMBER_CNT), ns = pkt_rd32(d + PKT_H_SYMBOL_CNT);
    printf("%s: %d member(s), %d symbol(s)\n", path, nm, ns);
    for (int i = 0; i < nm; i++) {
        unsigned char *r = d + PKT_HDR_SIZE + i * PKT_MEMBER_SIZE;
        const char *name = b + pkt_rd32(r + PKT_M_NAME_OFF);
        uint64_t caps = pkt_rd64(r + PKT_M_CAPS);
        printf("  %-16s %6u bytes  caps:", name, pkt_rd32(r + PKT_M_DATA_SIZE));
        if (!caps) printf(" -");
        for (unsigned c = 0; c < sizeof pkt_capnames/sizeof pkt_capnames[0]; c++)
            if (caps & pkt_capnames[c].bit) printf(" %s", pkt_capnames[c].name);
        printf("\n");
    }
    for (int i = 0; i < ns; i++) {
        unsigned char *r = d + PKT_HDR_SIZE + nm * PKT_MEMBER_SIZE + i * PKT_SYMBOL_SIZE;
        printf("    %s %s\n", r[PKT_S_KIND] == PKT_SYM_FUNC ? "T" : "D", b + pkt_rd32(r + PKT_S_NAME_OFF));
    }
    tcc_free(b);
    return 0;
}

/* Entry: `tcc -pkt OUT.PKT obj...`  or  `tcc -pkt list PKT`.
 * argv[0] is the "-pkt" token (as for tcc -ar); real args start at argv[1]. */
ST_FUNC int tcc_tool_pkt(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "list"))
        return pkt_list(argv[2]) < 0 ? 1 : 0;
    if (argc < 3) {
        fprintf(stderr, "usage: tcc -pkt OUT.PKT obj.o...   |   tcc -pkt list PKT\n");
        return 1;
    }
    return pkt_create(argv[1], argc - 2, argv + 2) < 0 ? 1 : 0;
}
