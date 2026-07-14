// Bare-metal I2C master for the Raspberry Pi 4 (BCM2711 BSC1) on the 40-pin
// header: SDA1 = BCM 2 (pin 3), SCL1 = BCM 3 (pin 5). The BSC ("Broadcom Serial
// Controller") is a hardware I2C engine with a 16-byte FIFO; we drive it in
// polled mode with a safety timeout so a stuck bus can never hang the machine.
//
// Real-hardware only. QEMU's raspi4b does not model the BSC at all (reads return
// 0, DONE never sets), so this is gated on g_real_hw exactly like the audio
// driver: i2c_available() is 0 under the emulator and the interpreter refuses
// the call before we ever touch a BSC register.

#include <stdint.h>
#include "gpio.h"
#include "i2c.h"

extern int g_real_hw;                  // set by the kernel; 1 real Pi, 0 QEMU

// BSC1 register block (BCM2711 peripheral low alias, base 0xFE000000).
#define BSC1_BASE 0xFE804000UL
#define BSC_C     (BSC1_BASE + 0x00)   // control
#define BSC_S     (BSC1_BASE + 0x04)   // status
#define BSC_DLEN  (BSC1_BASE + 0x08)   // data length (bytes to transfer)
#define BSC_A     (BSC1_BASE + 0x0C)   // slave address (7-bit)
#define BSC_FIFO  (BSC1_BASE + 0x10)   // data FIFO
#define BSC_DIV   (BSC1_BASE + 0x14)   // clock divider
#define ST_CLO    0xFE003004UL         // 1 MHz system timer (microseconds)

// Control-register bits.
#define C_I2CEN (1u << 15)             // enable the controller
#define C_ST    (1u << 7)              // start a transfer
#define C_CLEAR (1u << 4)             // clear the FIFO (one-shot)
#define C_READ  (1u << 0)              // 1 = read, 0 = write
// Status-register bits.
#define S_CLKT  (1u << 9)             // clock-stretch timeout
#define S_ERR   (1u << 8)             // slave did not acknowledge (NACK)
#define S_RXD   (1u << 5)             // RX FIFO holds data
#define S_TXD   (1u << 4)             // TX FIFO can accept data
#define S_DONE  (1u << 1)             // transfer complete

// Barriered 32-bit MMIO (a dsb on either side), like the GPIO driver: required
// when the CPU alternates between peripherals (here GPIO alt-select and the BSC).
static inline void wr(unsigned long a, uint32_t v) {
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)a = v;
    __asm__ volatile("dsb sy" ::: "memory");
}
static inline uint32_t rd(unsigned long a) {
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t v = *(volatile uint32_t *)a;
    __asm__ volatile("dsb sy" ::: "memory");
    return v;
}

static int inited = 0;

int i2c_available(void) { return g_real_hw ? 1 : 0; }

void i2c_reset(void) {
    if (g_real_hw && inited) wr(BSC_C, 0);     // disable the controller
    inited = 0;
}

// Route SDA1/SCL1 to the BSC (ALT0 on BCM 2/3), pick a clock, and enable it.
// gpio_reset() on RUN puts the pins back to inputs, so we re-init lazily.
static void ensure_init(void) {
    if (inited) return;
    gpio_set_mode(2, GPIO_ALT, 0);             // SDA1
    gpio_set_mode(3, GPIO_ALT, 0);             // SCL1
    // ~100 kHz standard mode. The BSC source clock is set by firmware and its
    // exact rate is not guaranteed, so we pick a conservative divider: this is
    // <=100 kHz for any plausible core clock, which every I2C device supports.
    wr(BSC_DIV, 2500);
    wr(BSC_C, C_I2CEN);
    inited = 1;
}

// Run one transaction to completion in polled mode, streaming the FIFO. Returns
// 0 on success, <0 on NACK / clock-stretch timeout / our 100 ms safety timeout.
static int xfer(int addr, unsigned char *buf, int n, int dir_read) {
    ensure_init();
    wr(BSC_A, (uint32_t)(addr & 0x7F));
    wr(BSC_C, C_I2CEN | C_CLEAR);              // empty the FIFO
    wr(BSC_S, S_CLKT | S_ERR | S_DONE);        // clear sticky status bits
    wr(BSC_DLEN, (uint32_t)n);

    int wi = 0, ri = 0;
    if (!dir_read)                             // pre-fill TX before starting
        while (wi < n && (rd(BSC_S) & S_TXD)) wr(BSC_FIFO, buf[wi++]);
    wr(BSC_C, C_I2CEN | C_ST | (dir_read ? C_READ : 0u));

    uint32_t t0 = rd(ST_CLO);
    for (;;) {
        uint32_t s = rd(BSC_S);
        if (dir_read) { if ((s & S_RXD) && ri < n) buf[ri++] = (unsigned char)rd(BSC_FIFO); }
        else          { if ((s & S_TXD) && wi < n) wr(BSC_FIFO, buf[wi++]); }
        if (s & (S_ERR | S_CLKT)) return -1;                 // NACK / stuck clock
        if (s & S_DONE) break;
        if (rd(ST_CLO) - t0 > 100000u) return -2;            // 100 ms: never hang
    }
    if (dir_read)                              // drain anything left in RX
        while (ri < n && (rd(BSC_S) & S_RXD)) buf[ri++] = (unsigned char)rd(BSC_FIFO);
    if (rd(BSC_S) & (S_ERR | S_CLKT)) return -1;
    return 0;
}

int i2c_write(int addr, const unsigned char *buf, int n) {
    if (!g_real_hw || n < 0) return -1;
    return xfer(addr, (unsigned char *)buf, n, 0);
}
int i2c_read(int addr, unsigned char *buf, int n) {
    if (!g_real_hw || n < 0) return -1;
    return xfer(addr, buf, n, 1);
}
int i2c_probe(int addr) {
    if (!g_real_hw) return 0;
    unsigned char b;
    return xfer(addr, &b, 1, 1) == 0 ? 1 : 0;  // a 1-byte read; address ACK = present
}
