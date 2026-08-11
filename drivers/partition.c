#include <stdint.h>
#include "partition.h"
#include "blockdev.h"
#include "fs.h"
#include "uart.h"

// ---------------------------------------------------------------------------
// Partition discovery: superfloppy / MBR / GPT. See partition.h. Fixed pools,
// no allocation. All multi-byte fields read byte-wise (-mstrict-align safe).
// ---------------------------------------------------------------------------

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static int sig8(const uint8_t *p, const char *lit) {
    for (int i = 0; i < 8; i++) if ((char)p[i] != lit[i]) return 0;
    return 1;
}

// CRC32 (IEEE 802.3, reflected) - used to validate the GPT header. Bitwise, no
// table, so no init and no BSS footprint.
static uint32_t crc32(const uint8_t *p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return c ^ 0xFFFFFFFFu;
}

// A definite filesystem boot sector at LBA 0 (superfloppy), by the BPB jump byte
// or an exFAT/NTFS signature. Deliberately NOT the loose BPB fallback, so a real
// MBR (whose entry area can resemble a BPB) is never mistaken for a filesystem.
static int looks_like_fs_boot(const uint8_t *s) {
    if (s[0] == 0xEB || s[0] == 0xE9) return 1;
    if (sig8(s + 3, "EXFAT   ") || sig8(s + 3, "NTFS    ")) return 1;
    return 0;
}

// Format a 16-byte GPT type GUID as text: first three fields little-endian,
// last two big-endian (the on-disk mixed-endian layout).
static void guid_text(const uint8_t *g, char out[37]) {
    static const char *hx = "0123456789ABCDEF";
    static const int order[16] = { 3,2,1,0, 5,4, 7,6, 8,9, 10,11,12,13,14,15 };
    int k = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[k++] = '-';
        uint8_t b = g[order[i]];
        out[k++] = hx[b >> 4];
        out[k++] = hx[b & 0x0F];
    }
    out[k] = 0;
}

static int emit_superfloppy(const uint8_t *s, uint64_t devblocks,
                            partition *out) {
    uint64_t len = devblocks;
    if (!len) {                                   // fall back to the BPB's total
        uint32_t tot16 = (uint32_t)(s[19] | (s[20] << 8));
        uint32_t tot32 = rd32(s + 32);
        len = tot16 ? tot16 : tot32;
    }
    out[0].start_lba = 0;
    out[0].nblocks = len;
    out[0].mbr_type = 0;
    out[0].gpt_type[0] = 0;
    out[0].is_superfloppy = 1;
    return 1;
}

// Parse the four MBR primary entries. Skips empty and extended (0x05/0x0F/0x85)
// entries. Assumes the caller already verified 0x55AA and sane status bytes.
static int scan_mbr(const uint8_t *s, partition *out, int max) {
    int n = 0;
    for (int i = 0; i < 4 && n < max; i++) {
        const uint8_t *e = s + 446 + i * 16;
        uint8_t   type  = e[4];
        uint32_t  start = rd32(e + 8);
        uint32_t  count = rd32(e + 12);
        if (type == 0 || count == 0) continue;
        if (type == 0x05 || type == 0x0F || type == 0x85) {
            uart_puts("[BLK] extended partition (EBR chain) skipped\n");
            continue;
        }
        out[n].start_lba = start;
        out[n].nblocks = count;
        out[n].mbr_type = type;
        out[n].gpt_type[0] = 0;
        out[n].is_superfloppy = 0;
        n++;
    }
    return n;
}

// Parse a GPT (header at LBA 1). Returns partitions found, or -1 if the header
// is not a valid GPT (caller may then treat the MBR as real).
static int scan_gpt(int blkdev, partition *out, int max) {
    uint8_t h[BLK_SECSZ];
    if (blk_read(blkdev, 1, 1, h)) return -1;
    if (!sig8(h, "EFI PART")) return -1;

    uint32_t hdr_size = rd32(h + 12);
    if (hdr_size < 92 || hdr_size > BLK_SECSZ) return -1;
    uint32_t stored_crc = rd32(h + 16);
    uint8_t tmp[BLK_SECSZ];
    for (uint32_t i = 0; i < hdr_size; i++) tmp[i] = h[i];
    tmp[16] = tmp[17] = tmp[18] = tmp[19] = 0;             // zero the CRC field
    if (crc32(tmp, hdr_size) != stored_crc) {
        uart_puts("[BLK] GPT header CRC mismatch\n");
        return -1;                                         // fall back to MBR
    }

    uint64_t ent_lba   = rd64(h + 0x48);
    uint32_t ent_count = rd32(h + 0x50);
    uint32_t ent_size  = rd32(h + 0x54);
    if (ent_size < 128 || ent_size > BLK_SECSZ) return -1;
    if (ent_count > 128) ent_count = 128;                  // sanity cap

    int n = 0;
    uint32_t per_sec = BLK_SECSZ / ent_size;
    uint8_t s[BLK_SECSZ];
    for (uint32_t idx = 0; idx < ent_count && n < max; ) {
        if (blk_read(blkdev, (uint32_t)ent_lba + idx / per_sec, 1, s)) break;
        for (uint32_t k = 0; k < per_sec && idx < ent_count && n < max; k++, idx++) {
            const uint8_t *e = s + k * ent_size;
            int empty = 1;
            for (int b = 0; b < 16; b++) if (e[b]) { empty = 0; break; }
            if (empty) continue;                           // unused entry
            uint64_t first = rd64(e + 0x20);
            uint64_t last  = rd64(e + 0x28);
            if (last < first) continue;
            out[n].start_lba = first;
            out[n].nblocks = last - first + 1;
            out[n].mbr_type = 0;
            guid_text(e, out[n].gpt_type);
            out[n].is_superfloppy = 0;
            n++;
        }
    }
    return n;
}

int part_scan(int blkdev, partition *out, int max) {
    if (max <= 0) return 0;
    blockdev *dev = blk_get(blkdev);
    uint64_t devblocks = dev ? dev->nblocks : 0;

    uint8_t s[BLK_SECSZ];
    if (blk_read(blkdev, 0, 1, s)) return 0;
    int have_55aa = (s[510] == 0x55 && s[511] == 0xAA);

    // Prefer a real partition table. A genuine MBR has 0x55AA and every entry's
    // status byte is 0x00 or 0x80; a filesystem superfloppy (which also carries
    // 0x55AA) fails that test because its "entries" are boot code.
    if (have_55aa) {
        int status_ok = 1;
        for (int i = 0; i < 4; i++) {
            uint8_t st = s[446 + i * 16];
            if (st != 0x00 && st != 0x80) { status_ok = 0; break; }
        }
        if (status_ok) {
            if (s[450] == 0xEE) {                          // protective MBR -> GPT
                int g = scan_gpt(blkdev, out, max);
                if (g >= 0) return g;
            }
            int n = scan_mbr(s, out, max);
            if (n > 0) return n;
        }
    }

    // No usable table: is LBA 0 itself a filesystem? (superfloppy)
    if (looks_like_fs_boot(s) || fs_identify(blkdev, 0) != FSID_UNKNOWN)
        return emit_superfloppy(s, devblocks, out);

    return 0;
}
