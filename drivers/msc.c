// USB Mass Storage Class driver: Bulk-Only Transport (BOT) + a minimal SCSI
// command set, over the generic xHCI bulk transport (Phase 2). Each attached
// BOT/SCSI stick is brought up and registered as a blockdev, so the Phase-1
// partition scanner and FAT driver read it exactly like the SD card.
//
// BOT is three bulk phases: a 31-byte Command Block Wrapper (out), an optional
// data phase, then a 13-byte Command Status Wrapper (in). SCSI multi-byte fields
// are BIG-endian; the CBW's own fields are little-endian. Real hardware only:
// QEMU does not emulate the VL805 xHCI. (USB storage Phase 3.)

#include <stdint.h>
#include "xhci.h"
#include "blockdev.h"
#include "msc.h"
#include "uart.h"

#define CBW_SIG  0x43425355u        // "USBC"
#define CSW_SIG  0x53425355u        // "USBS"

#define BOUNCE_SECTORS  64                          // per-transfer data chunk
#define BOUNCE_BYTES    (BOUNCE_SECTORS * BLK_SECSZ)

typedef struct {
    int      slot, ep_in, ep_out, lun;
    uint64_t nblocks;
} msc_dev;

static msc_dev  g_msc[BLK_MAX_DEV];
static int      g_msc_n;
static uint32_t g_tag = 1;

// Non-cached DMA scratch, allocated once from the xHCI arena (bulk buffers must
// come from there). g_cbw/g_csw are the wrappers; g_bounce carries data so the
// caller's (possibly cached) buffer never has to be DMA memory.
static uint8_t *g_cbw, *g_csw, *g_bounce;

static void bzero(uint8_t *p, int n)             { for (int i = 0; i < n; i++) p[i] = 0; }
static void bcopy(uint8_t *d, const uint8_t *s, uint32_t n) { for (uint32_t i = 0; i < n; i++) d[i] = s[i]; }

// One Bulk-Only Transport command. cdb/cdblen = the SCSI CDB; data (DMA) / datalen
// = the data phase; dir_in = 1 for device->host. Returns 0 on a passing CSW, <0 on
// any transport or command failure.
static int bot_xfer(msc_dev *m, const uint8_t *cdb, int cdblen,
                    void *data, int datalen, int dir_in) {
    uint8_t *c = g_cbw;
    bzero(c, 31);
    uint32_t tag = g_tag++;
    c[0] = 0x55; c[1] = 0x53; c[2] = 0x42; c[3] = 0x43;              // dCBWSignature "USBC"
    c[4] = tag; c[5] = tag >> 8; c[6] = tag >> 16; c[7] = tag >> 24; // dCBWTag
    c[8] = datalen; c[9] = datalen >> 8;                            // dCBWDataTransferLength (LE)
    c[10] = datalen >> 16; c[11] = datalen >> 24;
    c[12] = dir_in ? 0x80 : 0x00;                                   // bmCBWFlags
    c[13] = m->lun;                                                 // bCBWLUN
    c[14] = cdblen;                                                 // bCBWCBLength
    for (int i = 0; i < cdblen && i < 16; i++) c[15 + i] = cdb[i];  // CBWCB

    if (xhci_bulk_out(m->slot, c, 31, 2000) < 0) return -1;         // command phase

    if (datalen > 0) {                                             // data phase
        int r = dir_in ? xhci_bulk_in (m->slot, data, datalen, 5000)
                       : xhci_bulk_out(m->slot, data, datalen, 5000);
        if (r == -2) xhci_ep_reset(m->slot, dir_in ? m->ep_in : m->ep_out);  // stall: clear, get CSW
        else if (r < 0) return -2;
    }

    int r = xhci_bulk_in(m->slot, g_csw, 13, 2000);               // status phase
    if (r == -2) { xhci_ep_reset(m->slot, m->ep_in); r = xhci_bulk_in(m->slot, g_csw, 13, 2000); }
    if (r < 0) return -3;

    uint8_t *s = g_csw;
    uint32_t sig = s[0] | (s[1] << 8) | (s[2] << 16) | ((uint32_t)s[3] << 24);
    if (sig != CSW_SIG) return -4;
    if (s[12] != 0)     return -5;                                 // bCSWStatus != pass
    return 0;
}

// --- SCSI commands (CDB fields are big-endian) ------------------------------
static int scsi_tur(msc_dev *m) {                                 // TEST UNIT READY
    uint8_t cdb[6] = { 0, 0, 0, 0, 0, 0 };
    return bot_xfer(m, cdb, 6, 0, 0, 1);
}
static void scsi_request_sense(msc_dev *m) {                       // clear a CHECK CONDITION
    uint8_t cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    bot_xfer(m, cdb, 6, g_bounce, 18, 1);
}
static int scsi_inquiry(msc_dev *m, uint8_t *out36) {
    uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    int r = bot_xfer(m, cdb, 6, g_bounce, 36, 1);
    if (r == 0) bcopy(out36, g_bounce, 36);
    return r;
}
static int scsi_read_capacity(msc_dev *m, uint32_t *last_lba, uint32_t *blk_size) {
    uint8_t cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    int r = bot_xfer(m, cdb, 10, g_bounce, 8, 1);
    if (r) return r;
    *last_lba = ((uint32_t)g_bounce[0] << 24) | (g_bounce[1] << 16) | (g_bounce[2] << 8) | g_bounce[3];
    *blk_size = ((uint32_t)g_bounce[4] << 24) | (g_bounce[5] << 16) | (g_bounce[6] << 8) | g_bounce[7];
    return 0;
}
// Up to BOUNCE_SECTORS at a time; the blockdev ops below loop for more.
static int scsi_rw10(msc_dev *m, uint32_t lba, uint32_t n, void *buf, int write) {
    if (n > BOUNCE_SECTORS) n = BOUNCE_SECTORS;
    uint8_t cdb[10];
    cdb[0] = write ? 0x2A : 0x28; cdb[1] = 0;
    cdb[2] = lba >> 24; cdb[3] = lba >> 16; cdb[4] = lba >> 8; cdb[5] = lba;   // LBA (BE)
    cdb[6] = 0;
    cdb[7] = n >> 8; cdb[8] = n;                                              // block count (BE)
    cdb[9] = 0;
    if (write) {
        bcopy(g_bounce, buf, n * BLK_SECSZ);
        return bot_xfer(m, cdb, 10, g_bounce, n * BLK_SECSZ, 0);
    }
    int r = bot_xfer(m, cdb, 10, g_bounce, n * BLK_SECSZ, 1);
    if (r == 0) bcopy(buf, g_bounce, n * BLK_SECSZ);
    return r;
}

// --- blockdev ops -----------------------------------------------------------
static int msc_blk_read(void *ctx, uint32_t lba, uint32_t n, void *buf) {
    msc_dev *m = (msc_dev *)ctx;
    uint8_t *d = (uint8_t *)buf;
    while (n) {
        uint32_t chunk = n > BOUNCE_SECTORS ? BOUNCE_SECTORS : n;
        if (scsi_rw10(m, lba, chunk, d, 0) < 0) return -1;
        lba += chunk; n -= chunk; d += chunk * BLK_SECSZ;
    }
    return 0;
}
static int msc_blk_write(void *ctx, uint32_t lba, uint32_t n, const void *buf) {
    msc_dev *m = (msc_dev *)ctx;
    const uint8_t *s = (const uint8_t *)buf;
    while (n) {
        uint32_t chunk = n > BOUNCE_SECTORS ? BOUNCE_SECTORS : n;
        if (scsi_rw10(m, lba, chunk, (void *)s, 1) < 0) return -1;
        lba += chunk; n -= chunk; s += chunk * BLK_SECSZ;
    }
    return 0;
}

// GET MAX LUN (class request); unsupported = a single LUN.
static int get_max_lun(int slot, int iface) {
    uint8_t *b = (uint8_t *)xhci_dma_alloc(1, 64);
    if (xhci_control(slot, 0xA1, 0xFE, 0, (unsigned short)iface, 1, b) < 0) return 0;
    return b[0];
}

static const char *msc_names[BLK_MAX_DEV] = { "usb0", "usb1", "usb2", "usb3" };

int msc_init(void) {
    if (!g_cbw) {                                    // one-time DMA scratch
        g_cbw    = (uint8_t *)xhci_dma_alloc(64, 64);
        g_csw    = (uint8_t *)xhci_dma_alloc(64, 64);
        g_bounce = (uint8_t *)xhci_dma_alloc(BOUNCE_BYTES, 64);
        if (!g_cbw || !g_csw || !g_bounce) return 0;
    }

    int registered = 0, n = xhci_dev_count();
    for (int i = 0; i < n && g_msc_n < BLK_MAX_DEV; i++) {
        xhci_dev *d = xhci_dev_get(i);
        if (!d || !d->valid) continue;
        if (d->cls != 0x08 || d->proto != 0x50) continue;        // BOT/SCSI only
        if (!d->bulk_in || !d->bulk_out) continue;

        uart_dec("[MSC] mass storage on slot ", d->slot);
        if (xhci_bulk_config(d->slot, d->bulk_in, d->bulk_out, d->bulk_mps) < 0) {
            uart_puts("[MSC] bulk config failed\n"); continue;
        }

        msc_dev *m = &g_msc[g_msc_n];
        m->slot = d->slot; m->ep_in = d->bulk_in; m->ep_out = d->bulk_out; m->lun = 0;
        (void)get_max_lun(d->slot, d->ifnum);                    // LUN 0 only for now

        // Sticks report "not ready" for a moment after power-up; poll with a
        // REQUEST SENSE between attempts to clear the CHECK CONDITION.
        int ready = 0;
        for (int t = 0; t < 30; t++) {
            if (scsi_tur(m) == 0) { ready = 1; break; }
            scsi_request_sense(m);
            for (volatile int w = 0; w < 300000; w++) { }         // brief settle
        }
        if (!ready) { uart_puts("[MSC] unit not ready\n"); continue; }

        uint8_t inq[36];
        if (scsi_inquiry(m, inq) == 0) {
            char name[29];
            for (int k = 0; k < 28; k++) { uint8_t c = inq[8 + k]; name[k] = (c >= 32 && c < 127) ? (char)c : ' '; }
            name[28] = 0;
            uart_puts("[MSC] "); uart_puts(name); uart_puts("\n");
        }

        uint32_t last_lba = 0, bsz = 0;
        if (scsi_read_capacity(m, &last_lba, &bsz) != 0) { uart_puts("[MSC] read capacity failed\n"); continue; }
        uart_dec("[MSC] block count ", last_lba + 1);
        uart_dec("[MSC] block size  ", bsz);
        if (bsz != BLK_SECSZ) { uart_puts("[MSC] non-512 sector size, skipping\n"); continue; }
        m->nblocks = (uint64_t)last_lba + 1;

        blockdev bd;
        bd.name = msc_names[g_msc_n];
        bd.read = msc_blk_read; bd.write = msc_blk_write; bd.ctx = m;
        bd.nblocks = m->nblocks; bd.present = 1;
        int idx = blk_register(&bd);
        if (idx < 0) { uart_puts("[MSC] block pool full\n"); continue; }
        uart_dec("[MSC] registered as blockdev ", idx);
        g_msc_n++; registered++;
    }
    return registered;
}
