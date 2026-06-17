#include "mailbox.h"
#include "uart.h"
#include "mmu.h"

#define PERIPHERAL_BASE  0xFE000000UL
#define MBOX_BASE        (PERIPHERAL_BASE + 0x00B880)
#define MBOX_READ        (*(volatile uint32_t *)(MBOX_BASE + 0x00))
#define MBOX_STATUS      (*(volatile uint32_t *)(MBOX_BASE + 0x18))
#define MBOX_WRITE       (*(volatile uint32_t *)(MBOX_BASE + 0x20))
#define MBOX_EMPTY       0x40000000
#define MBOX_FULL        0x80000000
#define MBOX_CH_PROP     8

volatile uint32_t mbox[36] __attribute__((aligned(16)));

int mbox_call(void) {
    uint32_t r = (uint32_t)(((uintptr_t)mbox & ~0xFu) | MBOX_CH_PROP);

    // Push our request to RAM so the GPU (a separate cache domain) sees it.
    dcache_clean_inval((const void *)mbox, sizeof(mbox[0]) * 36);

    uint32_t spin = 0;
    while (MBOX_STATUS & MBOX_FULL) {
        if (++spin % 10000000 == 0)
            uart_puts("[MBOX] TX full, spinning...\n");
    }
    MBOX_WRITE = r;

    spin = 0;
    while (1) {
        while (MBOX_STATUS & MBOX_EMPTY) {
            if (++spin % 50000000 == 0)
                uart_puts("[MBOX] RX empty, spinning...\n");
        }
        uint32_t resp = MBOX_READ;
        if (resp == r) {
            // Drop stale cache lines so we read the GPU's response from RAM.
            dcache_clean_inval((const void *)mbox, sizeof(mbox[0]) * 36);
            return mbox[1] == 0x80000000;
        }
    }
}
