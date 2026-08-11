#include <stdint.h>
#include "fs.h"
#include "blockdev.h"
#include "uart.h"

// ---------------------------------------------------------------------------
// Filesystem identification, the driver registry, and volume table. See fs.h.
// All fixed pools, no allocation.
// ---------------------------------------------------------------------------

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// Compare 8 bytes at p with a literal (used for the FS signature strings).
static int sig8(const uint8_t *p, const char *lit) {
    for (int i = 0; i < 8; i++) if ((char)p[i] != lit[i]) return 0;
    return 1;
}

// --- filesystem identification ----------------------------------------------
fsid fs_identify(int blkdev, uint64_t start) {
    uint8_t s[BLK_SECSZ];
    if (blk_read(blkdev, (uint32_t)start, 1, s)) return FSID_UNKNOWN;

    // exFAT and NTFS carry a BPB-shaped area, so test them before the generic
    // FAT fallback or they get misidentified as FAT.
    if (sig8(s + 3, "EXFAT   ")) return FSID_EXFAT;
    if (sig8(s + 3, "NTFS    ")) return FSID_NTFS;

    int have_55aa = (s[510] == 0x55 && s[511] == 0xAA);
    if (have_55aa) {
        if (sig8(s + 82, "FAT32   ")) return FSID_FAT32;
        if (sig8(s + 54, "FAT12   ")) return FSID_FAT12;
        if (sig8(s + 54, "FAT16   ")) return FSID_FAT16;
    }

    // ext2/3/4: superblock at partition offset 1024, magic 0xEF53 at +56, i.e.
    // byte offset 1080 = sector (start+2), offset 56.
    uint8_t sb[BLK_SECSZ];
    if (!blk_read(blkdev, (uint32_t)start + 2, 1, sb)) {
        if (sb[56] == 0x53 && sb[57] == 0xEF) return FSID_EXT234;
    }

    // ISO9660 primary volume descriptor: "CD001" at byte 32769 = sector
    // (start+64), offset 1.
    uint8_t iso[BLK_SECSZ];
    if (!blk_read(blkdev, (uint32_t)start + 64, 1, iso)) {
        if (iso[1] == 'C' && iso[2] == 'D' && iso[3] == '0' &&
            iso[4] == '0' && iso[5] == '1') return FSID_ISO9660;
    }

    // Generic FAT fallback: a plausible BPB, classified by cluster count.
    if (have_55aa) {
        uint16_t bps = rd16(s + 11);
        uint8_t  spc = s[13];
        if (bps == 512 && spc && (spc & (spc - 1)) == 0) {
            uint16_t reserved = rd16(s + 14);
            uint8_t  nfats    = s[16];
            uint16_t root_ent = rd16(s + 17);
            uint16_t tot16    = rd16(s + 19);
            uint16_t fatsz16  = rd16(s + 22);
            uint32_t tot32    = rd32(s + 32);
            uint32_t fatsz32  = rd32(s + 36);
            uint32_t fatsz    = fatsz16 ? fatsz16 : fatsz32;
            uint32_t tot      = tot16 ? tot16 : tot32;
            uint32_t root_sec = ((uint32_t)root_ent * 32 + (BLK_SECSZ - 1)) / BLK_SECSZ;
            uint32_t meta     = reserved + (uint32_t)nfats * fatsz + root_sec;
            if (tot > meta && fatsz && nfats >= 1 && nfats <= 2) {
                uint32_t clusters = (tot - meta) / spc;
                if (clusters < 4085)       return FSID_FAT12;
                else if (clusters < 65525) return FSID_FAT16;
                else                       return FSID_FAT32;
            }
        }
    }
    return FSID_UNKNOWN;
}

const char *fs_id_name(fsid id) {
    switch (id) {
        case FSID_FAT12:   return "FAT12";
        case FSID_FAT16:   return "FAT16";
        case FSID_FAT32:   return "FAT32";
        case FSID_EXFAT:   return "exFAT";
        case FSID_NTFS:    return "NTFS";
        case FSID_EXT234:  return "ext2/3/4";
        case FSID_ISO9660: return "ISO9660";
        default:           return "unknown";
    }
}

// Which identified filesystems a driver can mount today. exFAT joins in Phase 5.
int fs_mountable(fsid id) {
    return id == FSID_FAT12 || id == FSID_FAT16 || id == FSID_FAT32;
}

// --- boot logging -----------------------------------------------------------
static void log_u64(uint64_t v) {
    char b[24]; int n = 0;
    if (!v) { uart_putc('0'); return; }
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) uart_putc(b[--n]);
}

// Print a byte count as "N.N UNIT" (one decimal), choosing KB/MB/GB/TB.
static void log_size(uint64_t bytes) {
    const char *unit = "bytes";
    uint64_t div = 1;
    if (bytes >= (1ull << 40)) { unit = "TB"; div = 1ull << 40; }
    else if (bytes >= (1ull << 30)) { unit = "GB"; div = 1ull << 30; }
    else if (bytes >= (1ull << 20)) { unit = "MB"; div = 1ull << 20; }
    else if (bytes >= (1ull << 10)) { unit = "KB"; div = 1ull << 10; }
    if (div == 1) { log_u64(bytes); uart_putc(' '); uart_puts(unit); return; }
    uint64_t whole = bytes / div;
    uint64_t frac  = ((bytes % div) * 10) / div;      // one decimal
    log_u64(whole); uart_putc('.'); log_u64(frac);
    uart_putc(' '); uart_puts(unit);
}

void blk_log_partition(int blkdev, int partidx, fsid id, const partition *p) {
    blockdev *d = blk_get(blkdev);
    uart_puts("[BLK] ");
    uart_puts(d ? d->name : "?");
    uart_putc('p'); log_u64((uint64_t)partidx + 1);
    uart_puts(": ");
    uart_puts(fs_id_name(id));
    uart_puts(", ");
    log_size(p->nblocks * (uint64_t)BLK_SECSZ);
    if (fs_mountable(id)) uart_puts(" - mountable\n");
    else if (id == FSID_UNKNOWN) uart_puts(" - no filesystem\n");
    else uart_puts(" - not supported (reformat to use it here)\n");
}

// --- driver registry --------------------------------------------------------
static const fs_driver *drivers[8];
static int              driver_n;

int fs_register(const fs_driver *d) {
    if (!d || driver_n >= (int)(sizeof drivers / sizeof drivers[0])) return -1;
    drivers[driver_n++] = d;
    return 0;
}

// --- volume table -----------------------------------------------------------
static fs_vol vols[FS_MAX_VOL];

fs_vol *fs_vol_get(int vol) {
    if (vol < 0 || vol >= FS_MAX_VOL || !vols[vol].used) return 0;
    return &vols[vol];
}
int fs_vol_count(void) {
    int n = 0;
    for (int i = 0; i < FS_MAX_VOL; i++) if (vols[i].used) n++;
    return n;
}

int fs_mount(int blkdev, uint64_t start, const char *volname) {
    int vi = -1;
    for (int i = 0; i < FS_MAX_VOL; i++) if (!vols[i].used) { vi = i; break; }
    if (vi < 0) return -1;

    for (int di = 0; di < driver_n; di++) {
        const fs_driver *d = drivers[di];
        if (!d->probe || !d->probe(blkdev, start)) continue;
        fs_vol *v = &vols[vi];
        v->drv = d; v->blkdev = blkdev; v->start = start; v->priv = 0;
        int k = 0; for (; volname && volname[k] && k < 7; k++) v->name[k] = volname[k];
        v->name[k] = 0;
        v->used = 1;
        if (d->mount && d->mount(v, blkdev, start) == 0) return vi;
        v->used = 0;                              // mount failed: free the slot
    }
    return -1;
}

int fs_unmount(int vol) {
    fs_vol *v = fs_vol_get(vol);
    if (!v) return -1;
    if (v->drv && v->drv->unmount) v->drv->unmount(v);
    v->used = 0;
    return 0;
}
