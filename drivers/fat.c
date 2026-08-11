#include <stdint.h>
#include "storage.h"
#include "sd.h"
#include "blockdev.h"
#include "fs.h"
#include "console.h"
#include "uart.h"

// ---------------------------------------------------------------------------
// FAT16 / FAT32 driver with subdirectory support (8.3 + VFAT long names).
// Handles paths ("DIR/SUB/FILE.EXT", absolute or relative to a current dir),
// MKDIR / RMDIR / CD. All multi-byte fields are read byte-wise to stay
// alignment-safe (-mstrict-align).
//
// It is a per-volume driver: every function takes a `fat_vol *V` and reaches
// the storage through the block layer (blk_read/blk_write on V->blkdev), so the
// SD card and a USB stick can each carry their own FAT volume. It registers an
// fs_driver ("fat"); the single-volume stg_* surface below drives the boot
// volume, and Phase 4 routes stg_* per volume through the same driver ops.
// ---------------------------------------------------------------------------

#define EOC32 0x0FFFFFF8u
#define EOC16 0xFFF8u
#define SECSZ 512
#define NAME_MAX 256
#define STG_MAX_FILES 4

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static void bzero512(uint8_t *p) { for (int i = 0; i < SECSZ; i++) p[i] = 0; }

// Accumulates VFAT long-name fragments as a directory is scanned in order.
typedef struct {
    char    name[NAME_MAX];   // reconstructed name, forward order, NUL-terminated
    int     want;             // fragment count promised by the "last" (0x40) entry
    int     seen;             // fragments actually gathered
    uint8_t cksum;            // short-name checksum they must all carry
    int     started;          // a 0x40 entry has opened a run
    int     ok;               // the run is still consistent
} lfn_acc;

// One open file channel: a 512-byte sector cache streaming to the volume.
typedef struct {
    int      used, writable;
    char     n83[11];
    uint32_t dir_lba;  int dir_off;   // directory entry location
    uint32_t first_clus, size, pos;
    uint32_t cur_clus, cur_cidx;      // last cluster walked to and its chain index
    uint32_t cache_lba; int cache_dirty;
    uint8_t  cache[SECSZ];            // cache_lba 0 == nothing cached (LBA 0 = MBR)
} stg_file;

// A mounted FAT volume: layout geometry, current directory, channel table and
// the single active directory-enumeration cursor.
typedef struct {
    int      blkdev;
    int      mounted;
    int      is_fat32;
    uint32_t part_lba;             // LBA where the volume (BPB) starts
    uint32_t sec_per_clus;
    uint32_t fat_begin;            // first FAT sector (absolute LBA)
    uint32_t fat_sectors;          // sectors per FAT
    uint32_t num_fats;
    uint32_t data_begin;           // sector of cluster 2 (absolute LBA)
    uint32_t root_dir_lba;         // FAT16 root directory start (absolute)
    uint32_t root_dir_sectors;     // FAT16 root directory size
    uint32_t root_cluster;         // FAT32 root cluster
    uint32_t total_clusters;
    // One-sector FAT cache: walking a cluster chain touches the same FAT sector
    // for many consecutive clusters (128 FAT32 entries per 512-byte sector), so
    // caching it turns a chain walk from one SD read per cluster into ~one per
    // 128 - the difference between a fast and a glacial multi-MB file read.
    uint32_t fatcache_lba;
    int      fatcache_valid;
    uint8_t  fatcache[SECSZ];
    uint32_t cwd_clus;             // current dir first cluster (0 == root)
    char     cwd_path[128];
    stg_file files[STG_MAX_FILES];
    // single active directory scan (DIROPEN / DIRNEXT)
    uint32_t enum_clus, enum_n;
    int      enum_e, enum_active;
    lfn_acc  enum_lfn;
} fat_vol;

static fat_vol fat_vols[FS_MAX_VOL];
static fat_vol *boot;                  // the boot volume backing the stg_* surface

// Parse and VALIDATE the BPB at the given absolute LBA into V. A corrupt boot
// sector must fail to mount rather than steer cluster arithmetic into nonsense.
static int fat_parse_bpb(fat_vol *V, uint32_t lba) {
    uint8_t s[SECSZ];
    if (blk_read(V->blkdev, lba, 1, s)) return STG_EIO;
    if (rd16(s + 510) != 0xAA55) return STG_ENOFS;

    uint16_t bytes_per_sec = rd16(s + 11);
    if (bytes_per_sec != SECSZ) return STG_ENOFS;
    uint32_t spc      = s[13];
    if (spc == 0 || spc > 128 || (spc & (spc - 1)) != 0) return STG_ENOFS;  // power of two 1..128
    uint16_t reserved = rd16(s + 14);
    uint32_t nfats    = s[16];
    if (nfats < 1 || nfats > 2) return STG_ENOFS;
    uint16_t root_ent = rd16(s + 17);
    uint16_t tot16    = rd16(s + 19);
    uint16_t fatsz16  = rd16(s + 22);
    uint32_t tot32    = rd32(s + 32);
    uint32_t fatsz32  = rd32(s + 36);

    uint32_t fat_sectors = fatsz16 ? fatsz16 : fatsz32;
    if (fat_sectors == 0) return STG_ENOFS;
    uint32_t root_dir_sectors = ((uint32_t)root_ent * 32 + (SECSZ - 1)) / SECSZ;
    uint32_t meta = reserved + nfats * fat_sectors + root_dir_sectors;
    uint32_t tot_sectors = tot16 ? tot16 : tot32;
    if (tot_sectors <= meta) return STG_ENOFS;              // would underflow

    V->part_lba         = lba;
    V->sec_per_clus     = spc;
    V->num_fats         = nfats;
    V->fat_sectors      = fat_sectors;
    V->fat_begin        = lba + reserved;
    V->root_dir_sectors = root_dir_sectors;
    V->root_dir_lba     = V->fat_begin + nfats * fat_sectors;
    V->data_begin       = V->root_dir_lba + root_dir_sectors;
    V->total_clusters   = (tot_sectors - meta) / spc;
    V->is_fat32         = (V->total_clusters >= 65525);
    V->root_cluster     = V->is_fat32 ? rd32(s + 44) : 0;
    if (V->is_fat32 && V->root_cluster < 2) return STG_ENOFS;
    return 0;
}

static uint32_t clus_to_lba(fat_vol *V, uint32_t c) {
    return V->data_begin + (c - 2) * V->sec_per_clus;
}

// Read the FAT entry for cluster `c` (the next-cluster link).
static uint32_t fat_get(fat_vol *V, uint32_t c) {
    uint32_t es = V->is_fat32 ? 4 : 2;
    uint32_t off = c * es;
    uint32_t lba = V->fat_begin + off / SECSZ;
    uint32_t idx = off % SECSZ;
    if (!V->fatcache_valid || V->fatcache_lba != lba) {
        if (blk_read(V->blkdev, lba, 1, V->fatcache)) return EOC32;
        V->fatcache_lba = lba; V->fatcache_valid = 1;
    }
    return V->is_fat32 ? (rd32(V->fatcache + idx) & 0x0FFFFFFFu) : rd16(V->fatcache + idx);
}

// Write the FAT entry for cluster `c` to every FAT copy.
static int fat_set(fat_vol *V, uint32_t c, uint32_t val) {
    uint32_t es = V->is_fat32 ? 4 : 2;
    uint32_t off = c * es;
    uint32_t idx = off % SECSZ;
    uint8_t s[SECSZ];
    for (uint32_t f = 0; f < V->num_fats; f++) {
        uint32_t lba = V->fat_begin + f * V->fat_sectors + off / SECSZ;
        if (blk_read(V->blkdev, lba, 1, s)) return STG_EIO;
        if (V->is_fat32) wr32(s + idx, (rd32(s + idx) & 0xF0000000u) | (val & 0x0FFFFFFFu));
        else             wr16(s + idx, (uint16_t)val);
        if (blk_write(V->blkdev, lba, 1, s)) return STG_EIO;
    }
    V->fatcache_valid = 0;              // FAT changed: drop the read cache
    return 0;
}

static int is_eoc(fat_vol *V, uint32_t v) { return V->is_fat32 ? (v >= EOC32) : (v >= EOC16); }

// Find a free cluster, mark it end-of-chain, return its number (0 = disk full).
static uint32_t fat_alloc(fat_vol *V) {
    for (uint32_t c = 2; c < V->total_clusters + 2; c++) {
        if (fat_get(V, c) == 0) {
            fat_set(V, c, V->is_fat32 ? EOC32 : EOC16);
            return c;
        }
    }
    return 0;
}

static void free_chain(fat_vol *V, uint32_t c) {
    while (c >= 2 && !is_eoc(V, c)) {
        uint32_t next = fat_get(V, c);
        fat_set(V, c, 0);
        c = next;
    }
}

// Absolute LBA of the n-th root-directory sector, or 0 past the end.
static uint32_t root_dir_sector(fat_vol *V, uint32_t n) {
    if (!V->is_fat32) return (n < V->root_dir_sectors) ? V->root_dir_lba + n : 0;
    uint32_t want_clus = n / V->sec_per_clus;
    uint32_t within    = n % V->sec_per_clus;
    uint32_t c = V->root_cluster;
    for (uint32_t i = 0; i < want_clus; i++) {
        c = fat_get(V, c);
        if (c < 2 || is_eoc(V, c)) return 0;
    }
    return clus_to_lba(V, c) + within;
}

// Absolute LBA of the n-th sector of directory `dir_clus` (0 == root), or 0.
static uint32_t dir_sector(fat_vol *V, uint32_t dir_clus, uint32_t n) {
    if (dir_clus == 0) return root_dir_sector(V, n);
    uint32_t want_clus = n / V->sec_per_clus;
    uint32_t within    = n % V->sec_per_clus;
    uint32_t c = dir_clus;
    for (uint32_t i = 0; i < want_clus; i++) {
        c = fat_get(V, c);
        if (c < 2 || is_eoc(V, c)) return 0;
    }
    return clus_to_lba(V, c) + within;
}

// Convert a BASIC filename ("PROG" or "PROG.BAS") to a padded 8.3 entry name.
static void name_to_83(const char *src, char out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int i = 0;
    while (*src && *src != '.' && i < 8) {
        char c = *src++;
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i++] = c;
    }
    while (*src && *src != '.') src++;        // skip overflow of base
    if (*src == '.') {
        src++;
        int j = 8;
        while (*src && j < 11) {
            char c = *src++;
            if (c >= 'a' && c <= 'z') c -= 32;
            out[j++] = c;
        }
    }
}

static int name_eq(const uint8_t *ent, const char n83[11]) {
    for (int i = 0; i < 11; i++) if ((char)ent[i] != n83[i]) return 0;
    return 1;
}

// First cluster stored in a 32-byte directory entry.
static uint32_t ent_cluster(const uint8_t *e) {
    return rd16(e + 26) | ((uint32_t)rd16(e + 20) << 16);
}

// --- VFAT long file names (read side) ---------------------------------------
// A long name is a run of 32-byte entries (attr 0x0F) placed directly *before*
// the file's 8.3 entry, in reverse order. Each fragment carries 13 UCS-2 chars
// and a checksum of the 8.3 name. We rebuild the name while scanning a directory
// forward, narrowing UCS-2 to the console's 8-bit set (code points >= 256 -> '_').

// Checksum of the 11-byte short name that every LFN fragment stores (byte 13).
static uint8_t lfn_checksum(const uint8_t n83[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = ((sum & 1) << 7) + (sum >> 1) + n83[i];
    return sum;
}

static void lfn_reset(lfn_acc *a) {
    a->started = 0; a->ok = 0; a->seen = 0; a->want = 0; a->name[0] = 0;
}

// Feed one 0x0F fragment.
static void lfn_feed(lfn_acc *a, const uint8_t *d) {
    uint8_t seq = d[0];
    if (seq == 0xE5) { lfn_reset(a); return; }           // deleted fragment
    if (seq & 0x40) {                                    // "last logical" = first seen
        lfn_reset(a);
        a->started = 1; a->ok = 1;
        a->want = seq & 0x1F;
        a->cksum = d[13];
        for (int i = 0; i < NAME_MAX; i++) a->name[i] = 0;
    }
    if (!a->started) return;                             // fragment with no opener
    int n = seq & 0x1F;
    if (n < 1 || n > 20 || d[13] != a->cksum) { a->ok = 0; return; }
    static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    int base = (n - 1) * 13;
    for (int i = 0; i < 13; i++) {
        uint16_t u = (uint16_t)d[pos[i]] | ((uint16_t)d[pos[i] + 1] << 8);
        char c;
        if (u == 0x0000 || u == 0xFFFF) c = 0;           // terminator / padding
        else c = (u < 256) ? (char)u : '_';
        int idx = base + i;
        if (idx < NAME_MAX - 1) a->name[idx] = c;
    }
    a->seen++;
}

// If the accumulated run is complete and matches the 8.3 name `n83`, copy the
// long name into out[NAME_MAX] and return 1; otherwise return 0.
static int lfn_take(const lfn_acc *a, const uint8_t n83[11], char *out) {
    if (!(a->started && a->ok && a->want > 0 && a->seen == a->want &&
          a->cksum == lfn_checksum(n83)))
        return 0;
    int i = 0; while (a->name[i] && i < NAME_MAX - 1) { out[i] = a->name[i]; i++; }
    out[i] = 0;
    return 1;
}

// ASCII case-insensitive string compare (Latin-1 accents compare exactly).
static int name_ci_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

// Build the printable 8.3 name of an entry into out (>= 13 bytes), honouring the
// VFAT lowercase flags (byte 12).
static void short_name(const uint8_t *d, char *out) {
    int k = 0;
    int lc_base = d[12] & 0x08, lc_ext = d[12] & 0x10;
    for (int i = 0; i < 8 && d[i] != ' '; i++) {
        char c = (char)d[i];
        if (lc_base && c >= 'A' && c <= 'Z') c += 32;
        out[k++] = c;
    }
    if (d[8] != ' ' || d[9] != ' ' || d[10] != ' ') {
        out[k++] = '.';
        for (int i = 8; i < 11 && d[i] != ' '; i++) {
            char c = (char)d[i];
            if (lc_ext && c >= 'A' && c <= 'Z') c += 32;
            out[k++] = c;
        }
    }
    out[k] = 0;
}

// Locate a directory entry within `dir_clus`, matching either the padded 8.3
// name `n83` (fast path) or, when `want` is non-NULL, a VFAT long name compared
// case-insensitively against `want`. On success fills *lba/*off with the 8.3
// entry's location and copies the 32-byte entry into ent. Returns 0, else <0.
static int dir_find_in(fat_vol *V, uint32_t dir_clus, const char n83[11], const char *want,
                       uint32_t *lba, int *off, uint8_t *ent) {
    uint8_t s[SECSZ];
    lfn_acc acc; lfn_reset(&acc);
    for (uint32_t n = 0; ; n++) {
        uint32_t l = dir_sector(V, dir_clus, n);
        if (!l) return STG_ENOTFOUND;
        if (blk_read(V->blkdev, l, 1, s)) return STG_EIO;
        for (int e = 0; e < SECSZ; e += 32) {
            uint8_t *d = s + e;
            if (d[0] == 0x00) return STG_ENOTFOUND;     // end of directory
            if (d[0] == 0xE5) { lfn_reset(&acc); continue; }      // deleted
            if (d[11] == 0x0F) { lfn_feed(&acc, d); continue; }   // LFN fragment
            int hit = name_eq(d, n83);
            if (!hit && want) {
                char lname[NAME_MAX];
                if (lfn_take(&acc, d, lname) && name_ci_eq(lname, want)) hit = 1;
            }
            if (hit) {
                if (lba) *lba = l;
                if (off) *off = e;
                if (ent) for (int i = 0; i < 32; i++) ent[i] = d[i];
                return 0;
            }
            lfn_reset(&acc);                            // this 8.3 entry wasn't ours
        }
    }
}

// Append a fresh, zeroed cluster to a directory's chain and return the LBA/off
// of its first (empty) slot. Returns 0, else <0.
static int grow_dir(fat_vol *V, uint32_t dir_clus, uint32_t *lba, int *off) {
    uint32_t chain = dir_clus;
    if (dir_clus == 0) {
        if (!V->is_fat32) return STG_EFULL;             // FAT16 root is fixed size
        chain = V->root_cluster;
    }
    uint32_t c = chain;                                 // walk to the last cluster
    for (;;) { uint32_t nx = fat_get(V, c); if (nx < 2 || is_eoc(V, nx)) break; c = nx; }
    uint32_t nc = fat_alloc(V);
    if (!nc) return STG_EFULL;
    if (fat_set(V, c, nc)) return STG_EIO;
    uint8_t z[SECSZ]; bzero512(z);
    uint32_t base = clus_to_lba(V, nc);
    for (uint32_t sc = 0; sc < V->sec_per_clus; sc++)
        if (blk_write(V->blkdev, base + sc, 1, z)) return STG_EIO;
    *lba = base; *off = 0;
    return 0;
}

// Find a free directory slot in `dir_clus`, growing the directory if it is full.
static int dir_find_free_in(fat_vol *V, uint32_t dir_clus, uint32_t *lba, int *off) {
    uint8_t s[SECSZ];
    for (uint32_t n = 0; ; n++) {
        uint32_t l = dir_sector(V, dir_clus, n);
        if (!l) return grow_dir(V, dir_clus, lba, off); // ran off the end -> extend
        if (blk_read(V->blkdev, l, 1, s)) return STG_EIO;
        for (int e = 0; e < SECSZ; e += 32)
            if (s[e] == 0x00 || s[e] == 0xE5) { *lba = l; *off = e; return 0; }
    }
}

// Write a fresh 32-byte directory entry into the slot at (lba, off). Uses a
// fixed 2026-01-01 timestamp (no RTC). Returns 0/err.
static int write_dir_entry(fat_vol *V, uint32_t lba, int off, const char n83[11],
                           uint8_t attr, uint32_t clus, uint32_t size) {
    uint8_t s[SECSZ];
    if (blk_read(V->blkdev, lba, 1, s)) return STG_EIO;
    uint8_t *e = s + off;
    for (int i = 0; i < 32; i++) e[i] = 0;
    for (int i = 0; i < 11; i++) e[i] = (uint8_t)n83[i];
    e[11] = attr;
    uint16_t fdate = (46 << 9) | (1 << 5) | 1;
    wr16(e + 14, 0); wr16(e + 16, fdate); wr16(e + 18, fdate);
    wr16(e + 22, 0); wr16(e + 24, fdate);
    wr16(e + 26, (uint16_t)(clus & 0xFFFF));
    wr16(e + 20, (uint16_t)((clus >> 16) & 0xFFFF));
    wr32(e + 28, size);
    return blk_write(V->blkdev, lba, 1, s) ? STG_EIO : 0;
}

// --- VFAT long file names (write side) --------------------------------------
static int sfc_ok(char c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    for (const char *p = "!#$%&'()-@^_`{}~"; *p; p++) if (c == *p) return 1;
    return 0;
}

// True if `name` fits a plain 8.3 entry with no loss.
static int name_is_83(const char *name) {
    int base = 0, ext = 0, seen_dot = 0, i = 0;
    for (i = 0; name[i]; i++) {
        char c = name[i];
        if (c == '.') { if (seen_dot) return 0; seen_dot = 1; continue; }
        char u = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        if (!sfc_ok(u)) return 0;
        if (!seen_dot) { if (++base > 8) return 0; }
        else           { if (++ext  > 3) return 0; }
    }
    if (i == 0 || base == 0) return 0;                  // "" or ".BAS"
    if (seen_dot && ext == 0) return 0;                 // "NAME."
    return 1;
}

// Build the basis + extension for a short alias from a long name.
static void short_basis(const char *name, char *basis, int *blen, char *ext, int *elen) {
    int n = 0; while (name[n]) n++;
    int last_dot = -1;
    for (int i = n - 1; i >= 0; i--) if (name[i] == '.') { last_dot = i; break; }
    int base_end = (last_dot >= 0) ? last_dot : n;
    int bl = 0, el = 0;
    for (int i = 0; i < base_end && bl < 8; i++) {
        char c = name[i];
        if (c == ' ' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        basis[bl++] = sfc_ok(c) ? c : '_';
    }
    if (bl == 0) basis[bl++] = '_';
    if (last_dot >= 0)
        for (int i = last_dot + 1; name[i] && el < 3; i++) {
            char c = name[i];
            if (c == ' ') continue;
            if (c >= 'a' && c <= 'z') c -= 32;
            ext[el++] = sfc_ok(c) ? c : '_';
        }
    *blen = bl; *elen = el;
}

// Choose a unique short alias "BASIS~n.EXT" for `name` within `parent`.
static int make_alias(fat_vol *V, uint32_t parent, const char *name, char out83[11]) {
    char basis[8], ext[3]; int bl, el;
    short_basis(name, basis, &bl, ext, &el);
    for (uint32_t seq = 1; seq < 1000000u; seq++) {
        char num[7]; int nl = 0; uint32_t v = seq;
        char tmp[7]; int tl = 0;
        do { tmp[tl++] = (char)('0' + v % 10); v /= 10; } while (v);
        for (int i = 0; i < tl; i++) num[i] = tmp[tl - 1 - i];
        nl = tl;
        int keep = 8 - 1 - nl;                          // basis chars before "~n"
        if (keep < 1) keep = 1;
        if (keep > bl) keep = bl;
        char e[11]; for (int i = 0; i < 11; i++) e[i] = ' ';
        int k = 0;
        for (int i = 0; i < keep; i++) e[k++] = basis[i];
        e[k++] = '~';
        for (int i = 0; i < nl; i++) e[k++] = num[i];
        for (int i = 0; i < el; i++) e[8 + i] = ext[i];
        if (dir_find_in(V, parent, e, 0, 0, 0, 0) == STG_ENOTFOUND) {
            for (int i = 0; i < 11; i++) out83[i] = e[i];
            return 0;
        }
    }
    return STG_EFULL;
}

// Read-modify-write one raw 32-byte directory entry into (lba, off).
static int put_raw_entry(fat_vol *V, uint32_t lba, int off, const uint8_t e[32]) {
    uint8_t s[SECSZ];
    if (blk_read(V->blkdev, lba, 1, s)) return STG_EIO;
    for (int i = 0; i < 32; i++) s[off + i] = e[i];
    return blk_write(V->blkdev, lba, 1, s) ? STG_EIO : 0;
}

// Find `count` consecutive free slots in `dir_clus`, growing the directory as
// needed. `count` is small (<= 21).
static int dir_find_run(fat_vol *V, uint32_t dir_clus, int count, uint32_t *lbas, int *offs) {
    int run = 0;
    uint8_t s[SECSZ];
    for (uint32_t n = 0; ; n++) {
        uint32_t l = dir_sector(V, dir_clus, n);
        if (!l) {
            uint32_t glba; int goff;                    // ran off the end: extend
            int r = grow_dir(V, dir_clus, &glba, &goff);
            if (r) return r;
            n--; continue;
        }
        if (blk_read(V->blkdev, l, 1, s)) return STG_EIO;
        for (int e = 0; e < SECSZ; e += 32) {
            if (s[e] == 0x00 || s[e] == 0xE5) {         // free slot
                lbas[run] = l; offs[run] = e;
                if (++run == count) return 0;
            } else {
                run = 0;                                // sequence broken; restart
            }
        }
    }
}

static const int lfn_pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

// Create a directory entry for `name`, writing an LFN run + short alias when the
// name doesn't fit 8.3, else a single plain 8.3 entry.
static int dir_create_entry(fat_vol *V, uint32_t parent, const char *name, uint8_t attr,
                            uint32_t clus, uint32_t size,
                            uint32_t *lba, int *off, char n83[11]) {
    if (name_is_83(name)) {
        name_to_83(name, n83);
        uint32_t dlba; int doff;
        int r = dir_find_free_in(V, parent, &dlba, &doff);
        if (r) return r;
        if ((r = write_dir_entry(V, dlba, doff, n83, attr, clus, size))) return r;
        if (lba) *lba = dlba;
        if (off) *off = doff;
        return 0;
    }

    int r = make_alias(V, parent, name, n83);
    if (r) return r;
    int len = 0; while (name[len]) len++;
    if (len > 255) len = 255;
    int nfrag = (len + 12) / 13;                        // 13 UCS-2 chars per entry
    int count = nfrag + 1;
    uint32_t lbas[21]; int offs[21];
    if ((r = dir_find_run(V, parent, count, lbas, offs))) return r;

    uint8_t cksum = lfn_checksum((const uint8_t *)n83);
    for (int i = 0; i < nfrag; i++) {                   // physical order = reverse seq
        int seq = nfrag - i;
        uint8_t f[32]; for (int j = 0; j < 32; j++) f[j] = 0;
        f[0]  = (uint8_t)(seq | (i == 0 ? 0x40 : 0));   // first physical = last logical
        f[11] = 0x0F; f[13] = cksum;                    // attr + checksum
        int b = (seq - 1) * 13;
        for (int j = 0; j < 13; j++) {
            int idx = b + j;
            uint16_t u = (idx < len) ? (uint16_t)(unsigned char)name[idx]
                       : (idx == len) ? 0x0000 : 0xFFFF;
            f[lfn_pos[j]] = (uint8_t)(u & 0xFF);
            f[lfn_pos[j] + 1] = (uint8_t)(u >> 8);
        }
        if ((r = put_raw_entry(V, lbas[i], offs[i], f))) return r;
    }
    if ((r = write_dir_entry(V, lbas[nfrag], offs[nfrag], n83, attr, clus, size))) return r;
    if (lba) *lba = lbas[nfrag];
    if (off) *off = offs[nfrag];
    return 0;
}

// Erase the 8.3 entry at (tgt_lba, tgt_off) together with its preceding LFN run.
static int dir_erase(fat_vol *V, uint32_t dir_clus, uint32_t tgt_lba, int tgt_off) {
    uint8_t s[SECSZ];
    uint32_t run_lba[20]; int run_off[20]; int run = 0;
    for (uint32_t n = 0; ; n++) {
        uint32_t l = dir_sector(V, dir_clus, n);
        if (!l) return STG_ENOTFOUND;
        if (blk_read(V->blkdev, l, 1, s)) return STG_EIO;
        for (int e = 0; e < SECSZ; e += 32) {
            uint8_t *d = s + e;
            if (d[0] == 0x00) return STG_ENOTFOUND;
            if (d[0] == 0xE5) { run = 0; continue; }
            if (d[11] == 0x0F) {                        // gather the current run
                if (d[0] & 0x40) run = 0;
                if (run < 20) { run_lba[run] = l; run_off[run] = e; run++; }
                continue;
            }
            if (l == tgt_lba && e == tgt_off) {         // the entry to erase
                for (int i = 0; i < run; i++) {
                    uint8_t t[SECSZ];
                    if (blk_read(V->blkdev, run_lba[i], 1, t)) return STG_EIO;
                    t[run_off[i]] = 0xE5;
                    if (blk_write(V->blkdev, run_lba[i], 1, t)) return STG_EIO;
                }
                if (blk_read(V->blkdev, l, 1, s)) return STG_EIO;   // re-read (fragment may share)
                s[e] = 0xE5;
                return blk_write(V->blkdev, l, 1, s) ? STG_EIO : 0;
            }
            run = 0;
        }
    }
}

// A directory holds no files (only "."/".." and free slots). Returns 1 if empty.
static int dir_is_empty(fat_vol *V, uint32_t dir_clus) {
    uint8_t s[SECSZ];
    for (uint32_t n = 0; ; n++) {
        uint32_t l = dir_sector(V, dir_clus, n);
        if (!l) return 1;                               // reached the end
        if (blk_read(V->blkdev, l, 1, s)) return 0;     // on error, refuse to remove
        for (int e = 0; e < SECSZ; e += 32) {
            uint8_t *d = s + e;
            if (d[0] == 0x00) return 1;                 // end of directory
            if (d[0] == 0xE5) continue;                 // deleted
            if (d[11] == 0x0F) continue;                // LFN
            if (d[0] == '.') continue;                  // "." / ".."
            return 0;                                   // a real entry -> not empty
        }
    }
}

// --- path resolution --------------------------------------------------------
static int next_comp(const char **pp, char comp[NAME_MAX]) {
    const char *p = *pp;
    while (*p == '/') p++;
    if (!*p) { *pp = p; return 0; }
    int i = 0;
    while (*p && *p != '/') { if (i < NAME_MAX - 1) comp[i++] = *p; p++; }
    comp[i] = 0;
    *pp = p;
    return 1;
}

// Move from directory `cur` into the child named by `comp` ("." / ".." handled).
static int descend(fat_vol *V, uint32_t cur, const char *comp, uint32_t *out) {
    if (comp[0] == '.' && comp[1] == 0) { *out = cur; return 0; }        // "."
    if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {              // ".."
        if (cur == 0) { *out = 0; return 0; }                           // root's parent
        char dd[11]; for (int i = 0; i < 11; i++) dd[i] = ' ';
        dd[0] = '.'; dd[1] = '.';
        uint8_t ent[32];
        int r = dir_find_in(V, cur, dd, 0, 0, 0, ent);
        if (r) return r;
        *out = ent_cluster(ent);                                        // 0 => root
        return 0;
    }
    char n83[11]; name_to_83(comp, n83);
    uint8_t ent[32];
    int r = dir_find_in(V, cur, n83, comp, 0, 0, ent);                  // match long names too
    if (r) return r;
    if (!(ent[11] & 0x10)) return STG_ENOTFOUND;                        // not a directory
    *out = ent_cluster(ent);
    return 0;
}

// Resolve a path to its parent directory cluster and the final component's 8.3
// name (the leaf need not exist; the intermediates must).
static int resolve_parent(fat_vol *V, const char *path, uint32_t *parent, char leaf83[11],
                          char *leaf_orig) {
    uint32_t cur = (path[0] == '/') ? 0 : V->cwd_clus;
    const char *p = path;
    char comp[NAME_MAX], pending[NAME_MAX];
    int have = 0;
    while (next_comp(&p, comp)) {
        if (have) {
            uint32_t nxt;
            int r = descend(V, cur, pending, &nxt);
            if (r) return r;
            cur = nxt;
        }
        int i = 0; for (; comp[i]; i++) pending[i] = comp[i]; pending[i] = 0;
        have = 1;
    }
    if (!have) return STG_ENOTFOUND;
    name_to_83(pending, leaf83);
    if (leaf_orig) { int i = 0; for (; pending[i]; i++) leaf_orig[i] = pending[i]; leaf_orig[i] = 0; }
    *parent = cur;
    return 0;
}

// --- whole-file read/write/delete -------------------------------------------
static int fatv_read(fat_vol *V, const char *name, char *buf, int maxlen) {
    if (!V->mounted) return STG_ENOFS;
    uint32_t parent; char leaf[11]; char leaf_orig[NAME_MAX];
    int r = resolve_parent(V, name, &parent, leaf, leaf_orig);
    if (r) return r;
    uint8_t ent[32];
    r = dir_find_in(V, parent, leaf, leaf_orig, 0, 0, ent);
    if (r) return r;
    if (ent[11] & 0x10) return STG_ENOTFOUND;           // it's a directory

    uint32_t clus = ent_cluster(ent);
    uint32_t size = rd32(ent + 28);
    if ((int)size > maxlen) size = maxlen;

    uint8_t s[SECSZ];
    uint32_t got = 0;
    while (clus >= 2 && !is_eoc(V, clus) && got < size) {
        uint32_t base = clus_to_lba(V, clus);
        for (uint32_t sc = 0; sc < V->sec_per_clus && got < size; sc++) {
            if (blk_read(V->blkdev, base + sc, 1, s)) return STG_EIO;
            for (int i = 0; i < SECSZ && got < size; i++) buf[got++] = (char)s[i];
        }
        clus = fat_get(V, clus);
    }
    return (int)got;
}

static int fatv_delete(fat_vol *V, const char *name) {
    if (!V->mounted) return STG_ENOFS;
    uint32_t parent; char leaf[11]; char leaf_orig[NAME_MAX];
    int r = resolve_parent(V, name, &parent, leaf, leaf_orig);
    if (r) return r;
    uint32_t lba; int off; uint8_t ent[32];
    r = dir_find_in(V, parent, leaf, leaf_orig, &lba, &off, ent);
    if (r) return r;
    if (ent[11] & 0x10) return STG_ENOTFOUND;           // use RMDIR for directories

    uint32_t clus = ent_cluster(ent);
    if (clus >= 2) free_chain(V, clus);
    return dir_erase(V, parent, lba, off);
}

static int fatv_write(fat_vol *V, const char *name, const char *data, int len) {
    if (!V->mounted) return STG_ENOFS;
    uint32_t parent; char n83[11]; char leaf_orig[NAME_MAX];
    int pr = resolve_parent(V, name, &parent, n83, leaf_orig);
    if (pr) return pr;

    uint32_t old_lba; int old_off; uint8_t old[32];
    if (dir_find_in(V, parent, n83, leaf_orig, &old_lba, &old_off, old) == 0) {
        if (old[11] & 0x10) return STG_EIO;             // a directory of that name
        uint32_t oc = ent_cluster(old);
        if (oc >= 2) free_chain(V, oc);
        dir_erase(V, parent, old_lba, old_off);
    }

    uint32_t first = 0, prev = 0;
    uint32_t written = 0;
    uint8_t s[SECSZ];

    while (written < (uint32_t)len) {
        uint32_t c = fat_alloc(V);
        if (!c) { if (first) free_chain(V, first); return STG_EFULL; }
        if (!first) first = c;
        if (prev)   fat_set(V, prev, c);
        prev = c;

        uint32_t base = clus_to_lba(V, c);
        for (uint32_t sc = 0; sc < V->sec_per_clus; sc++) {
            bzero512(s);
            for (int i = 0; i < SECSZ && written < (uint32_t)len; i++)
                s[i] = (uint8_t)data[written++];
            if (blk_write(V->blkdev, base + sc, 1, s)) { free_chain(V, first); return STG_EIO; }
        }
    }

    int r = dir_create_entry(V, parent, leaf_orig, 0x20, first, (uint32_t)len, 0, 0, n83);
    if (r) { if (first) free_chain(V, first); return r; }
    return 0;
}

// --- byte-level channel I/O -------------------------------------------------
static stg_file *handle(fat_vol *V, int ch) {
    if (ch < 1 || ch > STG_MAX_FILES) return 0;
    return V->files[ch - 1].used ? &V->files[ch - 1] : 0;
}

static int flush_cache(fat_vol *V, stg_file *f) {
    if (f->cache_lba && f->cache_dirty) {
        if (blk_write(V->blkdev, f->cache_lba, 1, f->cache)) return STG_EIO;
        f->cache_dirty = 0;
    }
    return 0;
}

// Load the sector holding byte f->pos into f->cache and return the byte offset.
static int cache_seek(fat_vol *V, stg_file *f, int for_write) {
    uint32_t bpc = V->sec_per_clus * SECSZ;
    uint32_t cidx = f->pos / bpc;
    uint32_t within = f->pos - cidx * bpc;
    uint32_t sec_in_clus = within / SECSZ;
    uint32_t off = within % SECSZ;

    uint32_t c, start_idx;
    if (f->first_clus < 2) {
        if (!for_write) return STG_EIO;
        c = fat_alloc(V);
        if (!c) return STG_EFULL;
        f->first_clus = c; f->cur_clus = c; f->cur_cidx = 0;
        start_idx = 0;
    } else if (f->cur_clus >= 2 && f->cur_cidx <= cidx) {
        c = f->cur_clus; start_idx = f->cur_cidx;   // continue walking forward
    } else {
        c = f->first_clus; start_idx = 0;           // seek backwards: restart
    }
    for (uint32_t i = start_idx; i < cidx; i++) {
        uint32_t nxt = fat_get(V, c);
        if (nxt < 2 || is_eoc(V, nxt)) {
            if (!for_write) return STG_EIO;
            nxt = fat_alloc(V);
            if (!nxt) return STG_EFULL;
            fat_set(V, c, nxt);
        }
        c = nxt;
    }
    f->cur_clus = c; f->cur_cidx = cidx;

    uint32_t lba = clus_to_lba(V, c) + sec_in_clus;
    if (f->cache_lba != lba) {
        int r = flush_cache(V, f);
        if (r) return r;
        if (blk_read(V->blkdev, lba, 1, f->cache)) return STG_EIO;
        f->cache_lba = lba;
        f->cache_dirty = 0;
    }
    return (int)off;
}

static int update_dir_entry(fat_vol *V, stg_file *f) {
    uint8_t s[SECSZ];
    if (blk_read(V->blkdev, f->dir_lba, 1, s)) return STG_EIO;
    uint8_t *e = s + f->dir_off;
    wr16(e + 26, (uint16_t)(f->first_clus & 0xFFFF));
    wr16(e + 20, (uint16_t)((f->first_clus >> 16) & 0xFFFF));
    wr32(e + 28, f->size);
    if (blk_write(V->blkdev, f->dir_lba, 1, s)) return STG_EIO;
    return 0;
}

static int fatv_open(fat_vol *V, const char *name, int mode) {
    if (!V->mounted) return 0;
    int idx = -1;
    for (int i = 0; i < STG_MAX_FILES; i++) if (!V->files[i].used) { idx = i; break; }
    if (idx < 0) return 0;                              // too many open channels
    stg_file *f = &V->files[idx];
    for (int i = 0; i < (int)sizeof(*f); i++) ((uint8_t *)f)[i] = 0;

    uint32_t parent; char n83[11]; char leaf_orig[NAME_MAX];
    if (resolve_parent(V, name, &parent, n83, leaf_orig)) return 0;  // bad path

    uint32_t lba; int off; uint8_t ent[32];
    int found = (dir_find_in(V, parent, n83, leaf_orig, &lba, &off, ent) == 0);
    if (found && (ent[11] & 0x10)) return 0;           // can't open a directory

    if (mode == STG_M_READ || mode == STG_M_UPDATE) {
        if (!found) return 0;                          // OPENIN/OPENUP need the file
        for (int i = 0; i < 11; i++) f->n83[i] = ent[i];
        f->dir_lba = lba; f->dir_off = off;
        f->first_clus = ent_cluster(ent);
        f->size = rd32(ent + 28);
        f->writable = (mode == STG_M_UPDATE);
    } else {                                           // STG_M_WRITE: create/truncate
        if (found) {                                   // drop old chain + entry (LFN run too)
            uint32_t oc = ent_cluster(ent);
            if (oc >= 2) free_chain(V, oc);
            dir_erase(V, parent, lba, off);
        }
        uint32_t dlba; int doff;
        if (dir_create_entry(V, parent, leaf_orig, 0x20, 0, 0, &dlba, &doff, n83)) return 0;
        for (int i = 0; i < 11; i++) f->n83[i] = n83[i];
        f->dir_lba = dlba; f->dir_off = doff;
        f->first_clus = 0; f->size = 0; f->writable = 1;
    }
    f->used = 1;
    return idx + 1;
}

static int fatv_close(fat_vol *V, int ch) {
    stg_file *f = handle(V, ch);
    if (!f) return STG_EBADF;
    int r = 0;
    if (f->writable) {
        if (flush_cache(V, f)) r = STG_EIO;
        if (update_dir_entry(V, f)) r = STG_EIO;
    }
    f->used = 0;
    return r;
}

static void fatv_close_all(fat_vol *V) {
    for (int i = 0; i < STG_MAX_FILES; i++)
        if (V->files[i].used) fatv_close(V, i + 1);
}

static int fatv_getb(fat_vol *V, int ch) {
    stg_file *f = handle(V, ch);
    if (!f) return STG_EBADF;
    if (f->pos >= f->size) return -1;                  // EOF
    int off = cache_seek(V, f, 0);
    if (off < 0) return off;
    int b = f->cache[off];
    f->pos++;
    return b;
}

// Bulk read: copy up to `n` bytes from the channel into buf, a whole sector-cache
// run at a time (far faster than one fatv_getb per byte for big reads like a WAD
// lump). Returns bytes read (short at EOF), or <0 on error.
static int fatv_readn(fat_vol *V, int ch, void *buf, int n) {
    stg_file *f = handle(V, ch);
    if (!f) return STG_EBADF;
    if (n < 0) return 0;
    uint8_t *out = (uint8_t *)buf;
    int got = 0;
    while (got < n && f->pos < f->size) {
        int off = cache_seek(V, f, 0);                 // load the sector for f->pos
        if (off < 0) return got > 0 ? got : off;
        int chunk = SECSZ - off;                       // rest of this cached sector
        if (chunk > n - got) chunk = n - got;
        uint32_t remain = f->size - f->pos;
        if ((uint32_t)chunk > remain) chunk = (int)remain;
        for (int i = 0; i < chunk; i++) out[got + i] = f->cache[off + i];
        got += chunk;
        f->pos += chunk;
    }
    return got;
}

static int fatv_putb(fat_vol *V, int ch, int byte) {
    stg_file *f = handle(V, ch);
    if (!f) return STG_EBADF;
    if (!f->writable) return STG_EBADF;
    int off = cache_seek(V, f, 1);
    if (off < 0) return off;
    f->cache[off] = (uint8_t)byte;
    f->cache_dirty = 1;
    f->pos++;
    if (f->pos > f->size) f->size = f->pos;
    return 0;
}

static long fatv_size(fat_vol *V, int ch) { stg_file *f = handle(V, ch); return f ? (long)f->size : STG_EBADF; }
static long fatv_tell(fat_vol *V, int ch) { stg_file *f = handle(V, ch); return f ? (long)f->pos  : STG_EBADF; }
static int  fatv_eof (fat_vol *V, int ch) { stg_file *f = handle(V, ch); return f ? (f->pos >= f->size) : 1; }

static int fatv_seek(fat_vol *V, int ch, long pos) {
    stg_file *f = handle(V, ch);
    if (!f) return STG_EBADF;
    if (pos < 0) pos = 0;
    if (pos > (long)f->size) pos = (long)f->size;      // can seek to EOF to append
    f->pos = (uint32_t)pos;
    return 0;
}

// List the current directory, marking subdirectories with "  <DIR>".
static void fatv_dir(fat_vol *V) {
    if (!V->mounted) { con_puts("No disk\n"); return; }
    uint8_t s[SECSZ];
    lfn_acc acc; lfn_reset(&acc);
    for (uint32_t n = 0; ; n++) {
        uint32_t l = dir_sector(V, V->cwd_clus, n);
        if (!l) return;
        if (blk_read(V->blkdev, l, 1, s)) { con_puts("Disk error\n"); return; }
        for (int e = 0; e < SECSZ; e += 32) {
            uint8_t *d = s + e;
            if (d[0] == 0x00) return;                    // end of directory
            if (d[0] == 0xE5) { lfn_reset(&acc); continue; }
            if (d[11] == 0x0F) { lfn_feed(&acc, d); continue; }   // LFN fragment
            if (d[11] & 0x08) { lfn_reset(&acc); continue; }     // volume label
            if (d[0] == '.')  { lfn_reset(&acc); continue; }      // "." / ".."
            char name[NAME_MAX];
            if (!lfn_take(&acc, d, name)) short_name(d, name);
            lfn_reset(&acc);
            con_puts(name);
            if (d[11] & 0x10) con_puts("  <DIR>");
            con_putc('\n');
        }
    }
}

static const char *fatv_cwd(fat_vol *V) { return V->cwd_path; }

static int fatv_mkdir(fat_vol *V, const char *path) {
    if (!V->mounted) return STG_ENOFS;
    uint32_t parent; char leaf[11]; char leaf_orig[NAME_MAX];
    int r = resolve_parent(V, path, &parent, leaf, leaf_orig);
    if (r) return r;
    if (dir_find_in(V, parent, leaf, leaf_orig, 0, 0, 0) == 0) return STG_EEXIST;

    uint32_t nc = fat_alloc(V);
    if (!nc) return STG_EFULL;
    uint8_t z[SECSZ]; bzero512(z);
    uint32_t base = clus_to_lba(V, nc);
    for (uint32_t sc = 0; sc < V->sec_per_clus; sc++)
        if (blk_write(V->blkdev, base + sc, 1, z)) { free_chain(V, nc); return STG_EIO; }

    char dot[11], dd[11];
    for (int i = 0; i < 11; i++) { dot[i] = ' '; dd[i] = ' '; }
    dot[0] = '.'; dd[0] = '.'; dd[1] = '.';
    if (write_dir_entry(V, base, 0,  dot, 0x10, nc, 0) ||
        write_dir_entry(V, base, 32, dd,  0x10, parent, 0)) { free_chain(V, nc); return STG_EIO; }

    char used83[11];
    if ((r = dir_create_entry(V, parent, leaf_orig, 0x10, nc, 0, 0, 0, used83))) {
        free_chain(V, nc); return r;
    }
    return 0;
}

static int fatv_rmdir(fat_vol *V, const char *path) {
    if (!V->mounted) return STG_ENOFS;
    uint32_t parent; char leaf[11]; char leaf_orig[NAME_MAX];
    int r = resolve_parent(V, path, &parent, leaf, leaf_orig);
    if (r) return r;
    uint32_t lba; int off; uint8_t ent[32];
    r = dir_find_in(V, parent, leaf, leaf_orig, &lba, &off, ent);
    if (r) return r;
    if (!(ent[11] & 0x10)) return STG_ENOTFOUND;         // not a directory
    uint32_t clus = ent_cluster(ent);
    if (!dir_is_empty(V, clus)) return STG_ENOTEMPTY;
    if (clus >= 2) free_chain(V, clus);
    return dir_erase(V, parent, lba, off);
}

// Adjust the display path for a "/COMP" push or a ".." pop.
static void path_pop(char *p) {
    int n = 0; while (p[n]) n++;
    if (n <= 1) return;                                  // already "/"
    n--; while (n > 0 && p[n] != '/') n--;
    if (n == 0) { p[0] = '/'; p[1] = 0; } else p[n] = 0;
}
static void path_push(char *p, const char *comp) {
    int n = 0; while (p[n]) n++;
    if (!(n == 1 && p[0] == '/') && n < 126) p[n++] = '/';
    for (int i = 0; comp[i] && n < 126; i++) {
        char c = comp[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        p[n++] = c;
    }
    p[n] = 0;
}

static int fatv_chdir(fat_vol *V, const char *path) {
    if (!V->mounted) return STG_ENOFS;
    uint32_t cur = (path[0] == '/') ? 0 : V->cwd_clus;
    char newpath[128];
    if (path[0] == '/') { newpath[0] = '/'; newpath[1] = 0; }
    else { int i = 0; for (; V->cwd_path[i]; i++) newpath[i] = V->cwd_path[i]; newpath[i] = 0; }

    const char *p = path; char comp[NAME_MAX];
    while (next_comp(&p, comp)) {
        uint32_t nxt;
        int r = descend(V, cur, comp, &nxt);
        if (r) return r;                                 // invalid: leave cwd unchanged
        cur = nxt;
        if (comp[0] == '.' && comp[1] == 0) { /* stay */ }
        else if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) path_pop(newpath);
        else path_push(newpath, comp);
        if (cur == 0) { newpath[0] = '/'; newpath[1] = 0; }   // landed back at root
    }
    V->cwd_clus = cur;
    int i = 0; for (; newpath[i] && i < 127; i++) V->cwd_path[i] = newpath[i];
    V->cwd_path[i] = 0;
    return 0;
}

// Resolve a whole path to a directory's first cluster (0 = root).
static int resolve_dir(fat_vol *V, const char *path, uint32_t *out) {
    uint32_t cur = (path[0] == '/') ? 0 : V->cwd_clus;
    const char *p = path; char comp[NAME_MAX];
    while (next_comp(&p, comp)) {
        uint32_t nxt;
        int r = descend(V, cur, comp, &nxt);
        if (r) return r;
        cur = nxt;
    }
    *out = cur;
    return 0;
}

static int fatv_diropen(fat_vol *V, const char *path) {
    if (!V->mounted) return STG_ENOFS;
    uint32_t clus;
    int r = resolve_dir(V, path, &clus);
    if (r) return r;
    V->enum_clus = clus; V->enum_n = 0; V->enum_e = 0; V->enum_active = 1;
    lfn_reset(&V->enum_lfn);
    return 0;
}

static int fatv_dirnext(fat_vol *V, stg_dirent *out) {
    if (!V->mounted) return STG_ENOFS;
    if (!V->enum_active) return STG_EBADF;
    uint8_t s[SECSZ];
    for (;;) {
        uint32_t l = dir_sector(V, V->enum_clus, V->enum_n);
        if (!l) { V->enum_active = 0; return 0; }              // ran off the end
        if (blk_read(V->blkdev, l, 1, s)) return STG_EIO;
        while (V->enum_e < SECSZ) {
            uint8_t *d = s + V->enum_e;
            V->enum_e += 32;
            if (d[0] == 0x00) { V->enum_active = 0; return 0; } // end-of-directory marker
            if (d[0] == 0xE5) { lfn_reset(&V->enum_lfn); continue; }   // deleted
            if (d[11] == 0x0F) { lfn_feed(&V->enum_lfn, d); continue; } // LFN fragment
            if (d[11] & 0x08) { lfn_reset(&V->enum_lfn); continue; }    // volume label
            if (d[0] == '.')  { lfn_reset(&V->enum_lfn); continue; }    // "." / ".."
            if (!lfn_take(&V->enum_lfn, d, out->name)) short_name(d, out->name);
            lfn_reset(&V->enum_lfn);
            out->is_dir = (d[11] & 0x10) ? 1 : 0;
            out->size   = (long)rd32(d + 28);
            uint16_t wt = rd16(d + 22), wd = rd16(d + 24);   // write time / date
            out->year   = 1980 + (wd >> 9);
            out->month  = (wd >> 5) & 0x0F;
            out->day    = wd & 0x1F;
            out->hour   = wt >> 11;
            out->minute = (wt >> 5) & 0x3F;
            return 1;
        }
        V->enum_n++; V->enum_e = 0;
    }
}

// ---------------------------------------------------------------------------
// fs_driver registration
// ---------------------------------------------------------------------------
static int fat_probe(int blkdev, uint64_t start) {
    fsid id = fs_identify(blkdev, start);
    return id == FSID_FAT12 || id == FSID_FAT16 || id == FSID_FAT32;
}

static int fat_mount(fs_vol *v, int blkdev, uint64_t start) {
    fat_vol *V = 0;                                     // grab a free volume slot
    for (int i = 0; i < FS_MAX_VOL; i++) if (!fat_vols[i].mounted) { V = &fat_vols[i]; break; }
    if (!V) return STG_ENOFS;
    for (int i = 0; i < (int)sizeof(*V); i++) ((uint8_t *)V)[i] = 0;
    V->blkdev = blkdev;
    int r = fat_parse_bpb(V, (uint32_t)start);
    if (r) return r;
    V->cwd_clus = 0;
    V->cwd_path[0] = '/'; V->cwd_path[1] = 0;
    V->mounted = 1;
    v->priv = V;
    uart_puts(V->is_fat32 ? "[FAT] mounted (FAT32)\n" : "[FAT] mounted (FAT16)\n");
    return 0;
}

static void fat_unmount(fs_vol *v) {
    fat_vol *V = (fat_vol *)v->priv;
    if (!V) return;
    fatv_close_all(V);
    V->mounted = 0;
    if (boot == V) boot = 0;
}

// fs_driver op adapters: cast the volume handle's private state to fat_vol*.
static int  op_read_all (fs_vol *v, const char *n, char *b, int m) { return fatv_read((fat_vol *)v->priv, n, b, m); }
static int  op_write_all(fs_vol *v, const char *n, const char *d, int l) { return fatv_write((fat_vol *)v->priv, n, d, l); }
static int  op_remove (fs_vol *v, const char *p) { return fatv_delete((fat_vol *)v->priv, p); }
static int  op_mkdir  (fs_vol *v, const char *p) { return fatv_mkdir((fat_vol *)v->priv, p); }
static int  op_rmdir  (fs_vol *v, const char *p) { return fatv_rmdir((fat_vol *)v->priv, p); }
static int  op_chdir  (fs_vol *v, const char *p) { return fatv_chdir((fat_vol *)v->priv, p); }
static const char *op_cwd(fs_vol *v) { return fatv_cwd((fat_vol *)v->priv); }
static void op_dir    (fs_vol *v) { fatv_dir((fat_vol *)v->priv); }
static int  op_diropen(fs_vol *v, const char *p) { return fatv_diropen((fat_vol *)v->priv, p); }
static int  op_dirnext(fs_vol *v, stg_dirent *o) { return fatv_dirnext((fat_vol *)v->priv, o); }
static int  op_open   (fs_vol *v, const char *n, int m) { return fatv_open((fat_vol *)v->priv, n, m); }
static int  op_close  (fs_vol *v, int ch) { return fatv_close((fat_vol *)v->priv, ch); }
static int  op_getb   (fs_vol *v, int ch) { return fatv_getb((fat_vol *)v->priv, ch); }
static int  op_putb   (fs_vol *v, int ch, int b) { return fatv_putb((fat_vol *)v->priv, ch, b); }
static long op_size   (fs_vol *v, int ch) { return fatv_size((fat_vol *)v->priv, ch); }
static long op_tell   (fs_vol *v, int ch) { return fatv_tell((fat_vol *)v->priv, ch); }
static int  op_seek   (fs_vol *v, int ch, long p) { return fatv_seek((fat_vol *)v->priv, ch, p); }
static int  op_eof    (fs_vol *v, int ch) { return fatv_eof((fat_vol *)v->priv, ch); }

static const fs_driver fat_driver = {
    .name = "fat",
    .probe = fat_probe, .mount = fat_mount, .unmount = fat_unmount,
    .read_all = op_read_all, .write_all = op_write_all, .remove = op_remove,
    .mkdir = op_mkdir, .rmdir = op_rmdir, .chdir = op_chdir, .cwd = op_cwd,
    .dir = op_dir, .diropen = op_diropen, .dirnext = op_dirnext,
    .open = op_open, .close = op_close, .getb = op_getb, .putb = op_putb,
    .size = op_size, .tell = op_tell, .seek = op_seek, .eof = op_eof,
};

void fat_register(void) { fs_register(&fat_driver); }

// ---------------------------------------------------------------------------
// SD card as blockdev 0, and stg_init: scan the card, log every partition, and
// mount the boot volume. The BerryBasiC card has two partitions and the user
// volume is #2, so "boot device: last mountable FAT" chooses it (and #1 on an
// old single-partition card). See partition.c / fs.c for the general machinery.
// ---------------------------------------------------------------------------
static int sd_blk_read (void *ctx, uint32_t lba, uint32_t n, void *buf) { (void)ctx; return sd_read (lba, n, buf); }
static int sd_blk_write(void *ctx, uint32_t lba, uint32_t n, const void *buf) { (void)ctx; return sd_write(lba, n, buf); }
static const blockdev sd_blockdev = { "sd0", sd_blk_read, sd_blk_write, 0, 0, 0 };

int stg_init(void) {
    boot = 0;
    if (sd_init()) { uart_puts("[FAT] SD init failed\n"); return STG_EIO; }
    fat_register();                                     /* and later exfat_register() */

    int b = blk_register(&sd_blockdev);
    if (b < 0) { uart_puts("[BLK] register failed\n"); return STG_EIO; }

    partition p[PART_MAX];
    int n = part_scan(b, p, PART_MAX);
    int chosen = -1;
    for (int i = 0; i < n; i++) {                        // boot device: last mountable
        fsid id = fs_identify(b, p[i].start_lba);
        blk_log_partition(b, i, id, &p[i]);
        if (fs_mountable(id)) chosen = i;
    }
    if (chosen < 0) { uart_puts("[FAT] no mountable filesystem\n"); return STG_ENOFS; }

    int vol = fs_mount(b, p[chosen].start_lba, "sd");
    if (vol < 0) { uart_puts("[FAT] mount failed\n"); return STG_ENOFS; }
    boot = (fat_vol *)fs_vol_get(vol)->priv;
    uart_puts("[BLK] sd0p"); uart_putc((char)('1' + chosen)); uart_puts(": mounted as sd\n");
    return 0;
}

// ---------------------------------------------------------------------------
// storage.h surface: single boot volume in Phase 1 (Phase 4 routes per volume).
// ---------------------------------------------------------------------------
int  stg_read (const char *name, char *buf, int maxlen) { return boot ? fatv_read(boot, name, buf, maxlen) : STG_ENOFS; }
int  stg_write(const char *name, const char *data, int len) { return boot ? fatv_write(boot, name, data, len) : STG_ENOFS; }
int  stg_delete(const char *name) { return boot ? fatv_delete(boot, name) : STG_ENOFS; }
void stg_dir(void) { if (boot) fatv_dir(boot); else con_puts("No disk\n"); }
int  stg_mkdir(const char *path) { return boot ? fatv_mkdir(boot, path) : STG_ENOFS; }
int  stg_rmdir(const char *path) { return boot ? fatv_rmdir(boot, path) : STG_ENOFS; }
int  stg_chdir(const char *path) { return boot ? fatv_chdir(boot, path) : STG_ENOFS; }
const char *stg_cwd(void) { return boot ? fatv_cwd(boot) : "/"; }
int  stg_diropen(const char *path) { return boot ? fatv_diropen(boot, path) : STG_ENOFS; }
int  stg_dirnext(stg_dirent *out) { return boot ? fatv_dirnext(boot, out) : STG_ENOFS; }
int  stg_open(const char *name, int mode) { return boot ? fatv_open(boot, name, mode) : 0; }
int  stg_close(int ch) { return boot ? fatv_close(boot, ch) : STG_EBADF; }
void stg_close_all(void) { if (boot) fatv_close_all(boot); }
int  stg_getb(int ch) { return boot ? fatv_getb(boot, ch) : STG_EBADF; }
int  stg_readn(int ch, void *buf, int n) { return boot ? fatv_readn(boot, ch, buf, n) : STG_EBADF; }
int  stg_putb(int ch, int byte) { return boot ? fatv_putb(boot, ch, byte) : STG_EBADF; }
long stg_size(int ch) { return boot ? fatv_size(boot, ch) : STG_EBADF; }
long stg_tell(int ch) { return boot ? fatv_tell(boot, ch) : STG_EBADF; }
int  stg_seek(int ch, long pos) { return boot ? fatv_seek(boot, ch, pos) : STG_EBADF; }
int  stg_eof(int ch) { return boot ? fatv_eof(boot, ch) : 1; }
