#include "blockdev.h"

// Fixed pool of block devices (see blockdev.h). No allocation; a registered
// device keeps `present = 1` until blk_unregister clears the slot.

static blockdev devs[BLK_MAX_DEV];
static int      dev_hi;                 // one past the highest slot ever used

int blk_register(const blockdev *dev) {
    if (!dev) return -1;
    for (int i = 0; i < BLK_MAX_DEV; i++) {
        if (!devs[i].present) {
            devs[i] = *dev;
            devs[i].present = 1;
            if (i + 1 > dev_hi) dev_hi = i + 1;
            return i;
        }
    }
    return -1;                          // pool full
}

void blk_unregister(int idx) {
    if (idx < 0 || idx >= BLK_MAX_DEV) return;
    devs[idx].present = 0;
    devs[idx].read = 0;
    devs[idx].write = 0;
}

blockdev *blk_get(int idx) {
    if (idx < 0 || idx >= BLK_MAX_DEV || !devs[idx].present) return 0;
    return &devs[idx];
}

int blk_count(void) { return dev_hi; }

int blk_read(int idx, uint32_t lba, uint32_t n, void *buf) {
    blockdev *d = blk_get(idx);
    if (!d || !d->read) return -1;
    if (n == 0) return 0;
    return d->read(d->ctx, lba, n, buf);
}

int blk_write(int idx, uint32_t lba, uint32_t n, const void *buf) {
    blockdev *d = blk_get(idx);
    if (!d || !d->write) return -1;
    if (n == 0) return 0;
    return d->write(d->ctx, lba, n, buf);
}
