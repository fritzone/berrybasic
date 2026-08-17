#ifndef BLOCKDEV_H
#define BLOCKDEV_H
#include <stdint.h>

// ---------------------------------------------------------------------------
// Block-device registry. A tiny indirection so more than one storage device
// (the SD card, a USB stick, ...) can present the same LBA/count/buffer block
// interface that fat.c and the partition scanner speak. Fixed pool, no malloc.
// Index 0 is the boot device (the SD card). Every buffer handed to read/write
// is BLK_SECSZ (512) bytes per sector.
// ---------------------------------------------------------------------------

#define BLK_SECSZ    512
#define BLK_MAX_DEV  6      // SD (index 0) + up to 5 USB sticks

typedef struct blockdev {
    const char *name;                                   // "sd0", "usb0"
    int  (*read )(void *ctx, uint32_t lba, uint32_t n, void *buf);
    int  (*write)(void *ctx, uint32_t lba, uint32_t n, const void *buf);
    void *ctx;                                          // driver private
    uint64_t nblocks;                                   // capacity in sectors, 0 = unknown
    int  present;                                       // 0 once the device is removed
} blockdev;

int       blk_register(const blockdev *dev);   // -> index, or <0 if the pool is full
void      blk_unregister(int idx);
blockdev *blk_get(int idx);                    // 0 if absent
int       blk_count(void);                     // highest registered slot + 1

// Sector I/O by device index. 0 on success, <0 on error (bad index / absent /
// device error). n == 0 is a successful no-op.
int blk_read (int idx, uint32_t lba, uint32_t n, void *buf);
int blk_write(int idx, uint32_t lba, uint32_t n, const void *buf);

#endif
