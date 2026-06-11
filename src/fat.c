#include <stdint.h>
#include "storage.h"
#include "sd.h"
#include "console.h"
#include "uart.h"

// ---------------------------------------------------------------------------
// Minimal FAT16 / FAT32 driver (root directory only, 8.3 names). Enough for
// BASIC LOAD/SAVE on the QEMU SD card. All multi-byte fields are read byte-wise
// to stay alignment-safe (-mstrict-align).
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
        uint32_t p = rd32(s + 454);           // first partition: start LBA
        if (p) base = p;
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

// Locate a directory entry by 8.3 name. On success fills *lba/*off with the
// entry's location and copies the 32-byte entry into ent. Returns 0, else <0.
static int dir_find(const char n83[11], uint32_t *lba, int *off, uint8_t *ent) {
    uint8_t s[SECSZ];
    for (uint32_t n = 0; ; n++) {
        uint32_t l = root_dir_sector(n);
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

// Find a free directory slot (deleted or never-used). Returns 0, else <0.
static int dir_find_free(uint32_t *lba, int *off) {
    uint8_t s[SECSZ];
    for (uint32_t n = 0; ; n++) {
        uint32_t l = root_dir_sector(n);
        if (!l) return STG_EFULL;                       // no room (FAT16 root is fixed)
        if (sd_read(l, 1, s)) return STG_EIO;
        for (int e = 0; e < SECSZ; e += 32) {
            if (s[e] == 0x00 || s[e] == 0xE5) { *lba = l; *off = e; return 0; }
        }
    }
}

int stg_read(const char *name, char *buf, int maxlen) {
    if (!fat_ok) return STG_ENOFS;
    char n83[11]; name_to_83(name, n83);
    uint8_t ent[32];
    int r = dir_find(n83, 0, 0, ent);
    if (r) return r;

    uint32_t clus = rd16(ent + 26) | ((uint32_t)rd16(ent + 20) << 16);
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
    char n83[11]; name_to_83(name, n83);
    uint32_t lba; int off; uint8_t ent[32];
    int r = dir_find(n83, &lba, &off, ent);
    if (r) return r;

    uint32_t clus = rd16(ent + 26) | ((uint32_t)rd16(ent + 20) << 16);
    if (clus >= 2) free_chain(clus);

    uint8_t s[SECSZ];
    if (sd_read(lba, 1, s)) return STG_EIO;
    s[off] = 0xE5;                                       // mark entry deleted
    if (sd_write(lba, 1, s)) return STG_EIO;
    return 0;
}

int stg_write(const char *name, const char *data, int len) {
    if (!fat_ok) return STG_ENOFS;
    char n83[11]; name_to_83(name, n83);

    // Overwrite: drop any existing file of the same name first.
    if (dir_find(n83, 0, 0, 0) == 0) stg_delete(name);

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
    int r = dir_find_free(&dlba, &doff);
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

void stg_dir(void) {
    if (!fat_ok) { con_puts("No disk\n"); return; }
    uint8_t s[SECSZ];
    int files = 0;
    for (uint32_t n = 0; ; n++) {
        uint32_t l = root_dir_sector(n);
        if (!l) break;
        if (sd_read(l, 1, s)) { con_puts("Disk error\n"); return; }
        for (int e = 0; e < SECSZ; e += 32) {
            uint8_t *d = s + e;
            if (d[0] == 0x00) return;                    // end of directory
            if (d[0] == 0xE5) continue;
            if (d[11] == 0x0F) continue;                 // LFN
            if (d[11] & 0x08) continue;                  // volume label
            // print "NAME.EXT"
            for (int i = 0; i < 8; i++) if (d[i] != ' ') con_putc((char)d[i]);
            if (d[8] != ' ' || d[9] != ' ' || d[10] != ' ') {
                con_putc('.');
                for (int i = 8; i < 11; i++) if (d[i] != ' ') con_putc((char)d[i]);
            }
            con_putc('\n');
            files++;
        }
    }
    (void)files;
}
