#include <stdint.h>
#include "storage.h"
#include "sd.h"
#include "console.h"
#include "uart.h"

// ---------------------------------------------------------------------------
// Minimal FAT16 / FAT32 driver with subdirectory support (8.3 names). Handles
// paths ("DIR/SUB/FILE.EXT", absolute or relative to a current directory),
// MKDIR / RMDIR / CD. All multi-byte fields are read byte-wise to stay
// alignment-safe (-mstrict-align).
// ---------------------------------------------------------------------------

static int      fat_ok;
static int      is_fat32;
static uint32_t part_lba;             // LBA where the volume (BPB) starts
static uint32_t sec_per_clus;
static uint32_t fat_begin;            // first FAT sector (absolute LBA)
static uint32_t fat_sectors;          // sectors per FAT
static uint32_t num_fats;
static uint32_t data_begin;           // sector of cluster 2 (absolute LBA)
static uint32_t root_dir_lba;         // FAT16 root directory start (absolute)
static uint32_t root_dir_sectors;     // FAT16 root directory size
static uint32_t root_cluster;         // FAT32 root cluster
static uint32_t total_clusters;

// Current working directory: cluster of its start (0 == root), plus a display
// path kept in sync for PWD / CAT. Subdirectories are supported via paths like
// "SPRITES/CAT.PNG"; a leading '/' means absolute from the root.
static uint32_t cwd_clus = 0;
static char     cwd_path[128] = "/";

#define EOC32 0x0FFFFFF8u
#define EOC16 0xFFF8u
#define SECSZ 512

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

// Parse the BPB at the given absolute LBA into the layout globals.
static int parse_bpb(uint32_t lba) {
    uint8_t s[SECSZ];
    if (sd_read(lba, 1, s)) return STG_EIO;
    if (rd16(s + 510) != 0xAA55) return STG_ENOFS;

    uint16_t bytes_per_sec = rd16(s + 11);
    if (bytes_per_sec != SECSZ) return STG_ENOFS;
    sec_per_clus     = s[13];
    uint16_t reserved = rd16(s + 14);
    num_fats         = s[16];
    uint16_t root_ent = rd16(s + 17);
    uint16_t tot16    = rd16(s + 19);
    uint16_t fatsz16  = rd16(s + 22);
    uint32_t tot32    = rd32(s + 32);
    uint32_t fatsz32  = rd32(s + 36);

    part_lba    = lba;
    fat_sectors = fatsz16 ? fatsz16 : fatsz32;
    fat_begin   = lba + reserved;
    root_dir_sectors = ((uint32_t)root_ent * 32 + (SECSZ - 1)) / SECSZ;
    root_dir_lba     = fat_begin + num_fats * fat_sectors;
    data_begin       = root_dir_lba + root_dir_sectors;

    uint32_t tot_sectors = tot16 ? tot16 : tot32;
    uint32_t data_sectors = tot_sectors - (reserved + num_fats * fat_sectors + root_dir_sectors);
    total_clusters = sec_per_clus ? data_sectors / sec_per_clus : 0;

    is_fat32   = (total_clusters >= 65525);
    root_cluster = is_fat32 ? rd32(s + 44) : 0;
    return 0;
}

int stg_init(void) {
    fat_ok = 0;
    if (sd_init()) { uart_puts("[FAT] SD init failed\n"); return STG_EIO; }

    // Decide superfloppy (BPB at LBA 0) vs MBR-partitioned.
    uint8_t s[SECSZ];
    if (sd_read(0, 1, s)) return STG_EIO;
    uint32_t base = 0;
    if (s[0] != 0xEB && s[0] != 0xE9) {       // not a BPB jump -> assume MBR
        // The BerryBasic card has two partitions: #1 is the boot/system volume
        // (firmware + kernel), #2 holds the user's BASIC programs. Mount #2 so
        // CAT/LOAD/SAVE only ever see user files. Fall back to #1 for old
        // single-partition cards. MBR entries are 16 bytes from 0x1BE; the
        // start-LBA field is at +8, so entry 1 -> 454, entry 2 -> 470.
        uint32_t p2 = rd32(s + 470);          // partition 2: start LBA
        uint32_t p1 = rd32(s + 454);          // partition 1: start LBA
        base = p2 ? p2 : p1;
    }
    int r = parse_bpb(base);
    if (r) { uart_puts("[FAT] no FAT filesystem\n"); return r; }

    fat_ok = 1;
    uart_puts(is_fat32 ? "[FAT] mounted (FAT32)\n" : "[FAT] mounted (FAT16)\n");
    return 0;
}

static uint32_t clus_to_lba(uint32_t c) { return data_begin + (c - 2) * sec_per_clus; }

// Read FAT entry for cluster `c` (the next-cluster link).
static uint32_t fat_get(uint32_t c) {
    uint32_t es = is_fat32 ? 4 : 2;
    uint32_t off = c * es;
    uint32_t lba = fat_begin + off / SECSZ;
    uint32_t idx = off % SECSZ;
    uint8_t s[SECSZ];
    if (sd_read(lba, 1, s)) return EOC32;
    return is_fat32 ? (rd32(s + idx) & 0x0FFFFFFFu) : rd16(s + idx);
}

// Write FAT entry for cluster `c` to every FAT copy.
static int fat_set(uint32_t c, uint32_t val) {
    uint32_t es = is_fat32 ? 4 : 2;
    uint32_t off = c * es;
    uint32_t idx = off % SECSZ;
    uint8_t s[SECSZ];
    for (uint32_t f = 0; f < num_fats; f++) {
        uint32_t lba = fat_begin + f * fat_sectors + off / SECSZ;
        if (sd_read(lba, 1, s)) return STG_EIO;
        if (is_fat32) wr32(s + idx, (rd32(s + idx) & 0xF0000000u) | (val & 0x0FFFFFFFu));
        else          wr16(s + idx, (uint16_t)val);
        if (sd_write(lba, 1, s)) return STG_EIO;
    }
    return 0;
}

static int is_eoc(uint32_t v) { return is_fat32 ? (v >= EOC32) : (v >= EOC16); }

// Find a free cluster, mark it end-of-chain, return its number (0 = disk full).
static uint32_t fat_alloc(void) {
    for (uint32_t c = 2; c < total_clusters + 2; c++) {
        if (fat_get(c) == 0) {
            fat_set(c, is_fat32 ? EOC32 : EOC16);
            return c;
        }
    }
    return 0;
}

static void free_chain(uint32_t c) {
    while (c >= 2 && !is_eoc(c)) {
        uint32_t next = fat_get(c);
        fat_set(c, 0);
        c = next;
    }
}

// Absolute LBA of the n-th root-directory sector, or 0 past the end.
static uint32_t root_dir_sector(uint32_t n) {
    if (!is_fat32) return (n < root_dir_sectors) ? root_dir_lba + n : 0;
    // FAT32: walk the root cluster chain.
    uint32_t want_clus = n / sec_per_clus;
    uint32_t within    = n % sec_per_clus;
    uint32_t c = root_cluster;
    for (uint32_t i = 0; i < want_clus; i++) {
        c = fat_get(c);
        if (c < 2 || is_eoc(c)) return 0;
    }
    return clus_to_lba(c) + within;
}

// Absolute LBA of the n-th sector of directory `dir_clus` (0 == root, which may
// be the FAT16 fixed area or the FAT32 root cluster chain), or 0 past the end.
static uint32_t dir_sector(uint32_t dir_clus, uint32_t n) {
    if (dir_clus == 0) return root_dir_sector(n);
    uint32_t want_clus = n / sec_per_clus;
    uint32_t within    = n % sec_per_clus;
    uint32_t c = dir_clus;
    for (uint32_t i = 0; i < want_clus; i++) {
        c = fat_get(c);
        if (c < 2 || is_eoc(c)) return 0;
    }
    return clus_to_lba(c) + within;
}

// Convert a BASIC filename ("PROG" or "PROG.BAS") to a padded 8.3 entry name.
static void name_to_83(const char *src, char out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int i = 0;
    // base name
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

// Locate a directory entry by 8.3 name within directory `dir_clus`. On success
// fills *lba/*off with the entry's location and copies the 32-byte entry into
// ent. Returns 0, else <0.
static int dir_find_in(uint32_t dir_clus, const char n83[11],
                       uint32_t *lba, int *off, uint8_t *ent) {
    uint8_t s[SECSZ];
    for (uint32_t n = 0; ; n++) {
        uint32_t l = dir_sector(dir_clus, n);
        if (!l) return STG_ENOTFOUND;
        if (sd_read(l, 1, s)) return STG_EIO;
        for (int e = 0; e < SECSZ; e += 32) {
            if (s[e] == 0x00) return STG_ENOTFOUND;     // end of directory
            if (s[e] == 0xE5) continue;                 // deleted
            if (s[e + 11] == 0x0F) continue;            // long-filename entry
            if (name_eq(s + e, n83)) {
                if (lba) *lba = l;
                if (off) *off = e;
                if (ent) for (int i = 0; i < 32; i++) ent[i] = s[e + i];
                return 0;
            }
        }
    }
}

// Append a fresh, zeroed cluster to a directory's chain and return the LBA/off
// of its first (empty) slot. `dir_clus` 0 means the root: the FAT16 fixed root
// cannot grow, but the FAT32 root is a normal chain. Returns 0, else <0.
static int grow_dir(uint32_t dir_clus, uint32_t *lba, int *off) {
    uint32_t chain = dir_clus;
    if (dir_clus == 0) {
        if (!is_fat32) return STG_EFULL;                // FAT16 root is fixed size
        chain = root_cluster;
    }
    uint32_t c = chain;                                 // walk to the last cluster
    for (;;) { uint32_t nx = fat_get(c); if (nx < 2 || is_eoc(nx)) break; c = nx; }
    uint32_t nc = fat_alloc();
    if (!nc) return STG_EFULL;
    if (fat_set(c, nc)) return STG_EIO;
    uint8_t z[SECSZ]; bzero512(z);
    uint32_t base = clus_to_lba(nc);
    for (uint32_t sc = 0; sc < sec_per_clus; sc++)
        if (sd_write(base + sc, 1, z)) return STG_EIO;
    *lba = base; *off = 0;
    return 0;
}

// Find a free directory slot in `dir_clus` (deleted or never-used), growing the
// directory if it is full. Returns 0, else <0.
static int dir_find_free_in(uint32_t dir_clus, uint32_t *lba, int *off) {
    uint8_t s[SECSZ];
    for (uint32_t n = 0; ; n++) {
        uint32_t l = dir_sector(dir_clus, n);
        if (!l) return grow_dir(dir_clus, lba, off);    // ran off the end -> extend
        if (sd_read(l, 1, s)) return STG_EIO;
        for (int e = 0; e < SECSZ; e += 32)
            if (s[e] == 0x00 || s[e] == 0xE5) { *lba = l; *off = e; return 0; }
    }
}

// Write a fresh 32-byte directory entry (name/attr/first-cluster/size) into the
// slot at (lba, off). Uses a fixed 2026-01-01 timestamp (no RTC). Returns 0/err.
static int write_dir_entry(uint32_t lba, int off, const char n83[11],
                           uint8_t attr, uint32_t clus, uint32_t size) {
    uint8_t s[SECSZ];
    if (sd_read(lba, 1, s)) return STG_EIO;
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
    return sd_write(lba, 1, s) ? STG_EIO : 0;
}

// A directory holds no files (only "."/".." and free slots). Returns 1 if empty.
static int dir_is_empty(uint32_t dir_clus) {
    uint8_t s[SECSZ];
    for (uint32_t n = 0; ; n++) {
        uint32_t l = dir_sector(dir_clus, n);
        if (!l) return 1;                               // reached the end
        if (sd_read(l, 1, s)) return 0;                 // on error, refuse to remove
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
// Copy the next '/'-separated component of *pp into comp (max 15 chars), advance
// *pp past it. Returns 1 if a component was read, 0 at end of the path.
static int next_comp(const char **pp, char comp[16]) {
    const char *p = *pp;
    while (*p == '/') p++;
    if (!*p) { *pp = p; return 0; }
    int i = 0;
    while (*p && *p != '/') { if (i < 15) comp[i++] = *p; p++; }
    comp[i] = 0;
    *pp = p;
    return 1;
}

// Move from directory `cur` into the child named by the raw component `comp`
// (handling "." and ".."). Sets *out to the child directory cluster (0 = root).
// Returns 0, or an error if the component is missing or is not a directory.
static int descend(uint32_t cur, const char *comp, uint32_t *out) {
    if (comp[0] == '.' && comp[1] == 0) { *out = cur; return 0; }        // "."
    if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {              // ".."
        if (cur == 0) { *out = 0; return 0; }                           // root's parent
        char dd[11]; for (int i = 0; i < 11; i++) dd[i] = ' ';
        dd[0] = '.'; dd[1] = '.';
        uint8_t ent[32];
        int r = dir_find_in(cur, dd, 0, 0, ent);
        if (r) return r;
        *out = ent_cluster(ent);                                        // 0 => root
        return 0;
    }
    char n83[11]; name_to_83(comp, n83);
    uint8_t ent[32];
    int r = dir_find_in(cur, n83, 0, 0, ent);
    if (r) return r;
    if (!(ent[11] & 0x10)) return STG_ENOTFOUND;                        // not a directory
    *out = ent_cluster(ent);
    return 0;
}

// Resolve a path to its parent directory cluster and the final component's 8.3
// name (the leaf need not exist; the intermediate directories must). Returns
// 0/err; STG_ENOTFOUND if the path has no final component (e.g. "" or "/").
static int resolve_parent(const char *path, uint32_t *parent, char leaf83[11]) {
    uint32_t cur = (path[0] == '/') ? 0 : cwd_clus;
    const char *p = path;
    char comp[16], pending[16];
    int have = 0;
    while (next_comp(&p, comp)) {
        if (have) {
            uint32_t nxt;
            int r = descend(cur, pending, &nxt);
            if (r) return r;
            cur = nxt;
        }
        int i = 0; for (; comp[i]; i++) pending[i] = comp[i]; pending[i] = 0;
        have = 1;
    }
    if (!have) return STG_ENOTFOUND;
    name_to_83(pending, leaf83);
    *parent = cur;
    return 0;
}

int stg_read(const char *name, char *buf, int maxlen) {
    if (!fat_ok) return STG_ENOFS;
    uint32_t parent; char leaf[11];
    int r = resolve_parent(name, &parent, leaf);
    if (r) return r;
    uint8_t ent[32];
    r = dir_find_in(parent, leaf, 0, 0, ent);
    if (r) return r;
    if (ent[11] & 0x10) return STG_ENOTFOUND;           // it's a directory

    uint32_t clus = ent_cluster(ent);
    uint32_t size = rd32(ent + 28);
    if ((int)size > maxlen) size = maxlen;

    uint8_t s[SECSZ];
    uint32_t got = 0;
    while (clus >= 2 && !is_eoc(clus) && got < size) {
        uint32_t base = clus_to_lba(clus);
        for (uint32_t sc = 0; sc < sec_per_clus && got < size; sc++) {
            if (sd_read(base + sc, 1, s)) return STG_EIO;
            for (int i = 0; i < SECSZ && got < size; i++) buf[got++] = (char)s[i];
        }
        clus = fat_get(clus);
    }
    return (int)got;
}

int stg_delete(const char *name) {
    if (!fat_ok) return STG_ENOFS;
    uint32_t parent; char leaf[11];
    int r = resolve_parent(name, &parent, leaf);
    if (r) return r;
    uint32_t lba; int off; uint8_t ent[32];
    r = dir_find_in(parent, leaf, &lba, &off, ent);
    if (r) return r;
    if (ent[11] & 0x10) return STG_ENOTFOUND;           // use RMDIR for directories

    uint32_t clus = ent_cluster(ent);
    if (clus >= 2) free_chain(clus);

    uint8_t s[SECSZ];
    if (sd_read(lba, 1, s)) return STG_EIO;
    s[off] = 0xE5;                                       // mark entry deleted
    if (sd_write(lba, 1, s)) return STG_EIO;
    return 0;
}

int stg_write(const char *name, const char *data, int len) {
    if (!fat_ok) return STG_ENOFS;
    uint32_t parent; char n83[11];
    int pr = resolve_parent(name, &parent, n83);
    if (pr) return pr;

    // Overwrite: drop any existing file of the same name first (refuse a dir).
    uint8_t old[32];
    if (dir_find_in(parent, n83, 0, 0, old) == 0) {
        if (old[11] & 0x10) return STG_EIO;             // a directory of that name
        stg_delete(name);
    }

    // Allocate and fill the cluster chain.
    uint32_t bytes_per_clus = sec_per_clus * SECSZ;
    uint32_t first = 0, prev = 0;
    uint32_t written = 0;
    uint8_t s[SECSZ];

    while (written < (uint32_t)len) {
        uint32_t c = fat_alloc();
        if (!c) { if (first) free_chain(first); return STG_EFULL; }
        if (!first) first = c;
        if (prev)   fat_set(prev, c);
        prev = c;

        uint32_t base = clus_to_lba(c);
        for (uint32_t sc = 0; sc < sec_per_clus; sc++) {
            bzero512(s);
            for (int i = 0; i < SECSZ && written < (uint32_t)len; i++)
                s[i] = (uint8_t)data[written++];
            if (sd_write(base + sc, 1, s)) { free_chain(first); return STG_EIO; }
        }
        (void)bytes_per_clus;
    }

    // Create the directory entry.
    uint32_t dlba; int doff;
    int r = dir_find_free_in(parent, &dlba, &doff);
    if (r) { if (first) free_chain(first); return r; }
    if (sd_read(dlba, 1, s)) { if (first) free_chain(first); return STG_EIO; }
    uint8_t *e = s + doff;
    for (int i = 0; i < 32; i++) e[i] = 0;
    for (int i = 0; i < 11; i++) e[i] = (uint8_t)n83[i];
    e[11] = 0x20;                                        // attribute: archive
    // Fixed timestamp 2026-01-01 00:00 (no RTC). Date = (year-1980)<<9|mon<<5|day.
    uint16_t fdate = (46 << 9) | (1 << 5) | 1;
    wr16(e + 14, 0);      wr16(e + 16, fdate);           // creation time/date
    wr16(e + 18, fdate);                                 // last-access date
    wr16(e + 22, 0);      wr16(e + 24, fdate);           // write time/date
    wr16(e + 26, (uint16_t)(first & 0xFFFF));            // first cluster low
    wr16(e + 20, (uint16_t)((first >> 16) & 0xFFFF));    // first cluster high
    wr32(e + 28, (uint32_t)len);                         // file size
    if (sd_write(dlba, 1, s)) { free_chain(first); return STG_EIO; }
    return 0;
}

// ---------------------------------------------------------------------------
// Byte-level channel (file-handle) I/O. Each open channel keeps one 512-byte
// sector cache and streams straight to the SD card: writes grow the cluster
// chain live and the directory size is written back on close. Sequential access
// is O(n) thanks to the cur_clus/cur_cidx forward-walk cache.
// ---------------------------------------------------------------------------
#define STG_MAX_FILES 4

typedef struct {
    int      used, writable;
    char     n83[11];
    uint32_t dir_lba;  int dir_off;   // directory entry location
    uint32_t first_clus, size, pos;
    uint32_t cur_clus, cur_cidx;      // last cluster walked to and its chain index
    uint32_t cache_lba; int cache_dirty;
    uint8_t  cache[SECSZ];            // 0 in cache_lba == nothing cached (LBA 0 = MBR)
} stg_file;

static stg_file files[STG_MAX_FILES];

static stg_file *handle(int ch) {
    if (ch < 1 || ch > STG_MAX_FILES) return 0;
    return files[ch - 1].used ? &files[ch - 1] : 0;
}

static int flush_cache(stg_file *f) {
    if (f->cache_lba && f->cache_dirty) {
        if (sd_write(f->cache_lba, 1, f->cache)) return STG_EIO;
        f->cache_dirty = 0;
    }
    return 0;
}

// Load the sector holding byte f->pos into f->cache and return the byte offset
// (0..511) within it. When for_write, missing clusters are allocated (growing
// the file); otherwise reading past the allocated chain is an error.
static int cache_seek(stg_file *f, int for_write) {
    uint32_t bpc = sec_per_clus * SECSZ;
    uint32_t cidx = f->pos / bpc;
    uint32_t within = f->pos - cidx * bpc;
    uint32_t sec_in_clus = within / SECSZ;
    uint32_t off = within % SECSZ;

    uint32_t c, start_idx;
    if (f->first_clus < 2) {
        if (!for_write) return STG_EIO;
        c = fat_alloc();
        if (!c) return STG_EFULL;
        f->first_clus = c; f->cur_clus = c; f->cur_cidx = 0;
        start_idx = 0;
    } else if (f->cur_clus >= 2 && f->cur_cidx <= cidx) {
        c = f->cur_clus; start_idx = f->cur_cidx;   // continue walking forward
    } else {
        c = f->first_clus; start_idx = 0;           // seek backwards: restart
    }
    for (uint32_t i = start_idx; i < cidx; i++) {
        uint32_t nxt = fat_get(c);
        if (nxt < 2 || is_eoc(nxt)) {
            if (!for_write) return STG_EIO;
            nxt = fat_alloc();
            if (!nxt) return STG_EFULL;
            fat_set(c, nxt);
        }
        c = nxt;
    }
    f->cur_clus = c; f->cur_cidx = cidx;

    uint32_t lba = clus_to_lba(c) + sec_in_clus;
    if (f->cache_lba != lba) {
        int r = flush_cache(f);
        if (r) return r;
        if (sd_read(lba, 1, f->cache)) return STG_EIO;
        f->cache_lba = lba;
        f->cache_dirty = 0;
    }
    return (int)off;
}

// Write the file's current first cluster + size back into its directory entry.
static int update_dir_entry(stg_file *f) {
    uint8_t s[SECSZ];
    if (sd_read(f->dir_lba, 1, s)) return STG_EIO;
    uint8_t *e = s + f->dir_off;
    wr16(e + 26, (uint16_t)(f->first_clus & 0xFFFF));
    wr16(e + 20, (uint16_t)((f->first_clus >> 16) & 0xFFFF));
    wr32(e + 28, f->size);
    if (sd_write(f->dir_lba, 1, s)) return STG_EIO;
    return 0;
}

int stg_open(const char *name, int mode) {
    if (!fat_ok) return 0;
    int idx = -1;
    for (int i = 0; i < STG_MAX_FILES; i++) if (!files[i].used) { idx = i; break; }
    if (idx < 0) return 0;                              // too many open channels
    stg_file *f = &files[idx];
    for (int i = 0; i < (int)sizeof(*f); i++) ((uint8_t *)f)[i] = 0;

    uint32_t parent; char n83[11];
    if (resolve_parent(name, &parent, n83)) return 0;  // bad path
    for (int i = 0; i < 11; i++) f->n83[i] = n83[i];

    uint32_t lba; int off; uint8_t ent[32];
    int found = (dir_find_in(parent, n83, &lba, &off, ent) == 0);
    if (found && (ent[11] & 0x10)) return 0;           // can't open a directory

    if (mode == STG_M_READ || mode == STG_M_UPDATE) {
        if (!found) return 0;                          // OPENIN/OPENUP need the file
        f->dir_lba = lba; f->dir_off = off;
        f->first_clus = ent_cluster(ent);
        f->size = rd32(ent + 28);
        f->writable = (mode == STG_M_UPDATE);
    } else {                                           // STG_M_WRITE: create/truncate
        if (found) stg_delete(name);                   // drop old chain + entry
        uint32_t dlba; int doff;
        if (dir_find_free_in(parent, &dlba, &doff)) return 0;
        uint8_t s[SECSZ];
        if (sd_read(dlba, 1, s)) return 0;
        uint8_t *e = s + doff;
        for (int i = 0; i < 32; i++) e[i] = 0;
        for (int i = 0; i < 11; i++) e[i] = (uint8_t)n83[i];
        e[11] = 0x20;                                  // archive
        uint16_t fdate = (46 << 9) | (1 << 5) | 1;     // 2026-01-01 (no RTC)
        wr16(e + 16, fdate); wr16(e + 18, fdate); wr16(e + 24, fdate);
        if (sd_write(dlba, 1, s)) return 0;
        f->dir_lba = dlba; f->dir_off = doff;
        f->first_clus = 0; f->size = 0; f->writable = 1;
    }
    f->used = 1;
    return idx + 1;
}

int stg_close(int ch) {
    stg_file *f = handle(ch);
    if (!f) return STG_EBADF;
    int r = 0;
    if (f->writable) {
        if (flush_cache(f)) r = STG_EIO;
        if (update_dir_entry(f)) r = STG_EIO;
    }
    f->used = 0;
    return r;
}

void stg_close_all(void) {
    for (int i = 0; i < STG_MAX_FILES; i++)
        if (files[i].used) stg_close(i + 1);
}

int stg_getb(int ch) {
    stg_file *f = handle(ch);
    if (!f) return STG_EBADF;
    if (f->pos >= f->size) return -1;                  // EOF
    int off = cache_seek(f, 0);
    if (off < 0) return off;
    int b = f->cache[off];
    f->pos++;
    return b;
}

int stg_putb(int ch, int byte) {
    stg_file *f = handle(ch);
    if (!f) return STG_EBADF;
    if (!f->writable) return STG_EBADF;
    int off = cache_seek(f, 1);
    if (off < 0) return off;
    f->cache[off] = (uint8_t)byte;
    f->cache_dirty = 1;
    f->pos++;
    if (f->pos > f->size) f->size = f->pos;
    return 0;
}

long stg_size(int ch) { stg_file *f = handle(ch); return f ? (long)f->size : STG_EBADF; }
long stg_tell(int ch) { stg_file *f = handle(ch); return f ? (long)f->pos  : STG_EBADF; }
int  stg_eof(int ch)  { stg_file *f = handle(ch); return f ? (f->pos >= f->size) : 1; }

int stg_seek(int ch, long pos) {
    stg_file *f = handle(ch);
    if (!f) return STG_EBADF;
    if (pos < 0) pos = 0;
    if (pos > (long)f->size) pos = (long)f->size;      // can seek to EOF to append
    f->pos = (uint32_t)pos;
    return 0;
}

// List the current directory, marking subdirectories with "  <DIR>".
void stg_dir(void) {
    if (!fat_ok) { con_puts("No disk\n"); return; }
    uint8_t s[SECSZ];
    for (uint32_t n = 0; ; n++) {
        uint32_t l = dir_sector(cwd_clus, n);
        if (!l) return;
        if (sd_read(l, 1, s)) { con_puts("Disk error\n"); return; }
        for (int e = 0; e < SECSZ; e += 32) {
            uint8_t *d = s + e;
            if (d[0] == 0x00) return;                    // end of directory
            if (d[0] == 0xE5) continue;
            if (d[11] == 0x0F) continue;                 // LFN
            if (d[11] & 0x08) continue;                  // volume label
            if (d[0] == '.') continue;                   // "." / ".."
            for (int i = 0; i < 8; i++) if (d[i] != ' ') con_putc((char)d[i]);
            if (d[8] != ' ' || d[9] != ' ' || d[10] != ' ') {
                con_putc('.');
                for (int i = 8; i < 11; i++) if (d[i] != ' ') con_putc((char)d[i]);
            }
            if (d[11] & 0x10) con_puts("  <DIR>");
            con_putc('\n');
        }
    }
}

const char *stg_cwd(void) { return cwd_path; }

int stg_mkdir(const char *path) {
    if (!fat_ok) return STG_ENOFS;
    uint32_t parent; char leaf[11];
    int r = resolve_parent(path, &parent, leaf);
    if (r) return r;
    if (dir_find_in(parent, leaf, 0, 0, 0) == 0) return STG_EEXIST;

    // Allocate and zero the new directory's first cluster.
    uint32_t nc = fat_alloc();
    if (!nc) return STG_EFULL;
    uint8_t z[SECSZ]; bzero512(z);
    uint32_t base = clus_to_lba(nc);
    for (uint32_t sc = 0; sc < sec_per_clus; sc++)
        if (sd_write(base + sc, 1, z)) { free_chain(nc); return STG_EIO; }

    // Seed it with "." (self) and ".." (parent; 0 when parent is the root).
    char dot[11], dd[11];
    for (int i = 0; i < 11; i++) { dot[i] = ' '; dd[i] = ' '; }
    dot[0] = '.'; dd[0] = '.'; dd[1] = '.';
    if (write_dir_entry(base, 0,  dot, 0x10, nc, 0) ||
        write_dir_entry(base, 32, dd,  0x10, parent, 0)) { free_chain(nc); return STG_EIO; }

    // Link it into the parent directory.
    uint32_t dlba; int doff;
    if ((r = dir_find_free_in(parent, &dlba, &doff))) { free_chain(nc); return r; }
    if ((r = write_dir_entry(dlba, doff, leaf, 0x10, nc, 0))) { free_chain(nc); return r; }
    return 0;
}

int stg_rmdir(const char *path) {
    if (!fat_ok) return STG_ENOFS;
    uint32_t parent; char leaf[11];
    int r = resolve_parent(path, &parent, leaf);
    if (r) return r;
    uint32_t lba; int off; uint8_t ent[32];
    r = dir_find_in(parent, leaf, &lba, &off, ent);
    if (r) return r;
    if (!(ent[11] & 0x10)) return STG_ENOTFOUND;         // not a directory
    uint32_t clus = ent_cluster(ent);
    if (!dir_is_empty(clus)) return STG_ENOTEMPTY;
    if (clus >= 2) free_chain(clus);
    uint8_t s[SECSZ];
    if (sd_read(lba, 1, s)) return STG_EIO;
    s[off] = 0xE5;                                        // mark entry deleted
    return sd_write(lba, 1, s) ? STG_EIO : 0;
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

int stg_chdir(const char *path) {
    if (!fat_ok) return STG_ENOFS;
    uint32_t cur = (path[0] == '/') ? 0 : cwd_clus;
    char newpath[128];
    if (path[0] == '/') { newpath[0] = '/'; newpath[1] = 0; }
    else { int i = 0; for (; cwd_path[i]; i++) newpath[i] = cwd_path[i]; newpath[i] = 0; }

    const char *p = path; char comp[16];
    while (next_comp(&p, comp)) {
        uint32_t nxt;
        int r = descend(cur, comp, &nxt);
        if (r) return r;                                 // invalid: leave cwd unchanged
        cur = nxt;
        if (comp[0] == '.' && comp[1] == 0) { /* stay */ }
        else if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) path_pop(newpath);
        else path_push(newpath, comp);
        if (cur == 0) { newpath[0] = '/'; newpath[1] = 0; }   // landed back at root
    }
    cwd_clus = cur;
    int i = 0; for (; newpath[i] && i < 127; i++) cwd_path[i] = newpath[i];
    cwd_path[i] = 0;
    return 0;
}
