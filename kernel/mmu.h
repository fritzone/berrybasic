#ifndef MMU_H
#define MMU_H

#include <stdint.h>

// Set up an identity-mapped translation table and enable the MMU plus the
// data and instruction caches. RAM is mapped Normal write-back cacheable;
// the peripheral region is mapped Device. Must be called once, early.
void mmu_init(void);

// Re-map the 2 MiB blocks covering [addr, addr+size) in the low 1 GiB as Normal
// non-cacheable. Used on real hardware to make the framebuffer (which the GPU
// scans out of RAM directly) coherent with CPU writes. A no-op for ranges above
// 1 GiB. Safe to call after mmu_init().
void mmu_set_noncached(uint64_t addr, uint64_t size);

// Clean and invalidate the data cache for [addr, addr+size) so a DMA/GPU agent
// sees CPU writes and the CPU sees agent writes (used around mailbox calls).
void dcache_clean_inval(const void *addr, uint64_t size);

// Identity-map the 1 GiB block containing `addr` as Device memory. Used to reach
// the PCIe outbound window (CPU address 0x6_00000000) which lies above the 4 GiB
// covered by the default mapping. Safe to call after mmu_init().
void mmu_map_device_block(uint64_t addr);

#endif
