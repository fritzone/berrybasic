#include <stdint.h>
#include "mmu.h"

// ---------------------------------------------------------------------------
// Identity-mapped MMU for the BCM2711 (Raspberry Pi 4).
//
// Level 1 maps the 4 GiB space as four 1 GiB entries. The first 1 GiB (where
// the GPU allocates the framebuffer) points to a level-2 table of 512 x 2 MiB
// blocks so individual regions can be made non-cacheable at runtime; the rest
// stay as 1 GiB block entries:
//     0x00000000-0x3FFFFFFF  RAM (2 MiB blocks) -> Normal WB, FB region -> NC
//     0x40000000-0x7FFFFFFF  RAM                -> Normal WB cacheable
//     0x80000000-0xBFFFFFFF  (above RAM)        -> Device
//     0xC0000000-0xFFFFFFFF  peripherals        -> Device
//
// Caches are enabled for the speed-up. Unlike QEMU, real hardware models the
// caches, so DMA/GPU-shared memory must be handled: the framebuffer is re-mapped
// non-cacheable (mmu_set_noncached) and mailbox buffers get cache maintenance
// (dcache_clean_inval).
// ---------------------------------------------------------------------------

#define MAIR_IDX_DEVICE     0       // attr0: Device-nGnRnE (0x00)
#define MAIR_IDX_NORMAL     1       // attr1: Normal WB WA  (0xFF)
#define MAIR_IDX_NORMAL_NC  2       // attr2: Normal non-cacheable (0x44)

#define DESC_BLOCK    (1ULL << 0)   // block entry (level 1/2)
#define DESC_TABLE    (3ULL << 0)   // table entry (points to next level)
#define DESC_AF       (1ULL << 10)  // access flag
#define DESC_SH_INNER (3ULL << 8)   // inner shareable
#define ATTR_IDX(n)   (((uint64_t)(n)) << 2)

#define BLK_2MB       0x200000ULL

static uint64_t l1_table[512] __attribute__((aligned(4096)));
static uint64_t l2_table[512] __attribute__((aligned(4096)));   // first 1 GiB, 2 MiB blocks

void mmu_init(void) {
    uint64_t normal_blk = DESC_BLOCK | DESC_AF | DESC_SH_INNER | ATTR_IDX(MAIR_IDX_NORMAL);
    uint64_t device_blk = DESC_BLOCK | DESC_AF | ATTR_IDX(MAIR_IDX_DEVICE);

    // Level 2: identity-map the first 1 GiB as 2 MiB Normal-cacheable blocks.
    for (int i = 0; i < 512; i++)
        l2_table[i] = ((uint64_t)i * BLK_2MB) | normal_blk;

    for (int i = 0; i < 512; i++) l1_table[i] = 0;
    l1_table[0] = (uint64_t)(uintptr_t)l2_table | DESC_TABLE;   // 0-1 GiB -> L2
    l1_table[1] = 0x40000000ULL | normal_blk;                  // RAM
    l1_table[2] = 0x80000000ULL | device_blk;                  // above RAM
    l1_table[3] = 0xC0000000ULL | device_blk;                  // peripherals

    // MAIR: attr0 Device, attr1 Normal WB, attr2 Normal NC.
    uint64_t mair = (0x00ULL << (8 * MAIR_IDX_DEVICE))    |
                    (0xFFULL << (8 * MAIR_IDX_NORMAL))    |
                    (0x44ULL << (8 * MAIR_IDX_NORMAL_NC));

    // T0SZ = 25 -> 39-bit VA (512 GiB) so we can reach the PCIe outbound window
    // at CPU address 0x6_00000000. The level-1 table stays the starting level.
    uint64_t tcr = (25ULL << 0)  |   // T0SZ -> 39-bit VA
                   (3ULL  << 8)  |   // IRGN0 = WB WA
                   (1ULL  << 10) |   // ORGN0 = WB WA
                   (3ULL  << 12) |   // SH0   = inner shareable
                   (0ULL  << 14) |   // TG0   = 4 KiB
                   (1ULL  << 23) |   // EPD1  (no TTBR1 walks)
                   (2ULL  << 32);    // IPS   = 40-bit

    __asm__ volatile("msr mair_el1,  %0" :: "r"(mair));
    __asm__ volatile("msr tcr_el1,   %0" :: "r"(tcr));
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)(uintptr_t)l1_table));
    __asm__ volatile("isb");

    __asm__ volatile("dsb ish");
    __asm__ volatile("tlbi vmalle1");
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 0)    // M  - enable MMU
          |  (1ULL << 2)    // C  - data cache
          |  (1ULL << 12);  // I  - instruction cache
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb");
}

void mmu_set_noncached(uint64_t addr, uint64_t size) {
    if (addr >= 0x40000000ULL) return;             // only the L2-mapped first 1 GiB
    uint64_t nc = DESC_BLOCK | DESC_AF | DESC_SH_INNER | ATTR_IDX(MAIR_IDX_NORMAL_NC);
    uint64_t start = addr & ~(BLK_2MB - 1);
    uint64_t end   = (addr + size + BLK_2MB - 1) & ~(BLK_2MB - 1);
    for (uint64_t a = start; a < end && a < 0x40000000ULL; a += BLK_2MB)
        l2_table[a >> 21] = a | nc;

    __asm__ volatile("dsb ish");
    __asm__ volatile("tlbi vmalle1");
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");
}

void mmu_map_device_block(uint64_t addr) {
    uint64_t device_blk = DESC_BLOCK | DESC_AF | ATTR_IDX(MAIR_IDX_DEVICE);
    uint64_t idx = addr >> 30;                 // 1 GiB block index
    if (idx >= 512) return;
    l1_table[idx] = (idx << 30) | device_blk;
    __asm__ volatile("dsb ish");
    __asm__ volatile("tlbi vmalle1");
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");
}

void dcache_clean_inval(const void *addr, uint64_t size) {
    // Cache line size = 4 << CTR_EL0.DminLine (in words).
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4ULL << ((ctr >> 16) & 0xF);
    uint64_t p   = (uint64_t)(uintptr_t)addr & ~(line - 1);
    uint64_t end = (uint64_t)(uintptr_t)addr + size;
    for (; p < end; p += line)
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb sy");
    __asm__ volatile("isb");
}
