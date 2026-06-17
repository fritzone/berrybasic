#ifndef SD_H
#define SD_H

#include <stdint.h>

// Minimal SD-card driver for the BCM2711 EMMC2 (Arasan SDHCI) controller, as
// emulated by QEMU's raspi4b. PIO transfers, 512-byte blocks.
//
// sd_init()    : initialise the card. Returns 0 on success, <0 on error.
// sd_read(...) : read `count` 512-byte blocks starting at LBA `lba` into buf.
// sd_write(...): write `count` 512-byte blocks. Both return 0 on success.

int sd_init(void);
int sd_read(uint32_t lba, uint32_t count, void *buf);
int sd_write(uint32_t lba, uint32_t count, const void *buf);

#endif
