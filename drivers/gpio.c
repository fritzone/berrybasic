// Bare-metal digital GPIO for the Raspberry Pi 4 (BCM2711): drive and read the
// 40-pin header (BCM 0..27) for LEDs, buttons and, via alt functions, the bus
// peripherals (I2C/SPI/UART/PWM).
//
// Numbering is BCM. Every register offset below is defined in BCM2711 terms and
// only the low peripheral alias (base 0xFE000000) is used, so this is Pi 4 only
// (a Pi 3 lives at 0x3F200000, a Pi 1 at 0x20200000).
//
// Two things about the BCM2711 differ from earlier Pis and from most tutorials:
//   * The pull resistors are set through GPIO_PUP_PDN_CNTRL_REG0..3 (2 bits per
//     pin), NOT the old GPPUD + GPPUDCLK "clock-in" dance, which is gone.
//   * A data memory barrier (dsb) is required around MMIO when the CPU switches
//     between different peripherals; the barriered accessors below make sure it
//     is never forgotten.
//
// Unlike the audio driver, plain digital reads/writes are harmless registers
// that QEMU's raspi4b models, so this is NOT gated on g_real_hw: it works under
// the emulator too (gpio_available() reports 1 on both).

#include <stdint.h>
#include "gpio.h"

#define GPIO_BASE 0xFE200000UL

// Register offsets (see the BCM2711 peripherals datasheet, GPIO section).
#define GPFSEL0   (GPIO_BASE + 0x00)   // function select, 10 pins/reg, 3 bits/pin
#define GPSET0    (GPIO_BASE + 0x1C)   // write 1-bits to drive pins high (atomic)
#define GPCLR0    (GPIO_BASE + 0x28)   // write 1-bits to drive pins low  (atomic)
#define GPLEV0    (GPIO_BASE + 0x34)   // read pin levels
#define GPEDS0    (GPIO_BASE + 0x40)   // edge-event detect status (write 1 to clear)
#define GPREN0    (GPIO_BASE + 0x4C)   // rising-edge detect enable
#define GPFEN0    (GPIO_BASE + 0x58)   // falling-edge detect enable
#define PUP_PDN0  (GPIO_BASE + 0xE4)   // pull control, 2 bits/pin (00 none 01 up 10 down)

// BCM2711 system timer: a free-running 1 MHz counter (microseconds). A different
// peripheral from GPIO, so the barriered accessors below keep the ordering safe.
#define ST_CLO    (0xFE003004UL)

#define GPIO_MAX_PIN 27                // highest reachable header pin (BCM)

// All register access is volatile 32-bit with a dsb on either side, so a switch
// to/from another peripheral can never see a reordered access (Broadcom's rule).
static inline void mmio_write32(unsigned long addr, uint32_t val) {
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)addr = val;
    __asm__ volatile("dsb sy" ::: "memory");
}
static inline uint32_t mmio_read32(unsigned long addr) {
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t v = *(volatile uint32_t *)addr;
    __asm__ volatile("dsb sy" ::: "memory");
    return v;
}

// FSEL 3-bit codes indexed by [mode][alt]. Input/output ignore alt; GPIO_ALT
// maps alt 0..5 through the datasheet's non-obvious ordering.
//   in=000 out=001 alt0=100 alt1=101 alt2=110 alt3=111 alt4=011 alt5=010
static const uint8_t alt_fsel[6] = { 0b100, 0b101, 0b110, 0b111, 0b011, 0b010 };

int gpio_available(void) { return 1; }

static int pin_ok(int pin) { return pin >= 0 && pin <= GPIO_MAX_PIN; }

int gpio_set_mode(int pin, int mode, int alt) {
    if (!pin_ok(pin)) return -1;
    uint32_t code;
    if (mode == GPIO_OUT)      code = 0b001;
    else if (mode == GPIO_ALT) { if (alt < 0 || alt > 5) return -1; code = alt_fsel[alt]; }
    else                       code = 0b000;                 // GPIO_IN / default
    unsigned long reg = GPFSEL0 + (unsigned long)(pin / 10) * 4;
    int shift = (pin % 10) * 3;
    uint32_t v = mmio_read32(reg);
    v &= ~(0x7u << shift);
    v |= code << shift;
    mmio_write32(reg, v);
    return 0;
}

int gpio_set_pull(int pin, int pull) {
    if (!pin_ok(pin)) return -1;
    uint32_t bits = (pull == GPIO_PULL_UP) ? 0b01u
                  : (pull == GPIO_PULL_DOWN) ? 0b10u : 0b00u;
    unsigned long reg = PUP_PDN0 + (unsigned long)(pin / 16) * 4;
    int shift = (pin % 16) * 2;
    uint32_t v = mmio_read32(reg);
    v &= ~(0x3u << shift);
    v |= bits << shift;
    mmio_write32(reg, v);
    return 0;
}

void gpio_write(int pin, int level) {
    if (!pin_ok(pin)) return;
    mmio_write32(level ? GPSET0 : GPCLR0, 1u << pin);
}

int gpio_read(int pin) {
    if (!pin_ok(pin)) return 0;
    return (mmio_read32(GPLEV0) >> pin) & 1u;
}

void gpio_set_mask(uint32_t mask) { mmio_write32(GPSET0, mask & 0x0FFFFFFFu); }
void gpio_clr_mask(uint32_t mask) { mmio_write32(GPCLR0, mask & 0x0FFFFFFFu); }
uint32_t gpio_read_all(void)      { return mmio_read32(GPLEV0) & 0x0FFFFFFFu; }

void gpio_reset(void) {
    for (int pin = 0; pin <= GPIO_MAX_PIN; pin++) {
        gpio_set_mode(pin, GPIO_IN, 0);
        gpio_set_pull(pin, GPIO_PULL_NONE);
    }
    // Disarm any edge detection left over from a previous program and clear the
    // whole latch, so PINWAIT always starts from a clean slate.
    mmio_write32(GPREN0, 0);
    mmio_write32(GPFEN0, 0);
    mmio_write32(GPEDS0, 0x0FFFFFFFu);
}

int gpio_wait_edge(int pin, int edge, int timeout_cs) {
    if (!pin_ok(pin)) return -1;
    uint32_t bit = 1u << pin;
    unsigned long en_reg = edge ? GPREN0 : GPFEN0;

    // Arm edge detection for just this pin (leave the others as they were), then
    // clear any stale latched event so we only see edges from here on.
    uint32_t saved = mmio_read32(en_reg);
    mmio_write32(en_reg, saved | bit);
    mmio_write32(GPEDS0, bit);

    uint32_t t0 = mmio_read32(ST_CLO);
    uint32_t limit = (timeout_cs > 0) ? (uint32_t)timeout_cs * 10000u : 0;
    int result = -1;
    for (;;) {
        if (mmio_read32(GPEDS0) & bit) { result = pin; break; }
        if ((mmio_read32(ST_CLO) - t0) >= limit) break;   // limit==0 -> poll once
    }

    // Restore the previous detect-enable state and clear our latch on the way out.
    mmio_write32(en_reg, saved);
    mmio_write32(GPEDS0, bit);
    return result;
}

// Non-blocking edge detection for the event system (ON PIN). gpio_arm_edge turns
// on rising and/or falling detect for a pin and clears any stale latch;
// gpio_poll_edge returns 1 (and clears the latch) if an edge has occurred since
// the last poll, catching transient pulses that happened between polls;
// gpio_disarm_edge turns detection back off. edge: 0 falling, 1 rising, 2 both.
void gpio_arm_edge(int pin, int edge) {
    if (!pin_ok(pin)) return;
    uint32_t bit = 1u << pin;
    if (edge == 1 || edge == 2) mmio_write32(GPREN0, mmio_read32(GPREN0) | bit);
    else                        mmio_write32(GPREN0, mmio_read32(GPREN0) & ~bit);
    if (edge == 0 || edge == 2) mmio_write32(GPFEN0, mmio_read32(GPFEN0) | bit);
    else                        mmio_write32(GPFEN0, mmio_read32(GPFEN0) & ~bit);
    mmio_write32(GPEDS0, bit);                       // clear any stale latch
}

int gpio_poll_edge(int pin) {
    if (!pin_ok(pin)) return 0;
    uint32_t bit = 1u << pin;
    if (mmio_read32(GPEDS0) & bit) { mmio_write32(GPEDS0, bit); return 1; }
    return 0;
}

void gpio_disarm_edge(int pin) {
    if (!pin_ok(pin)) return;
    uint32_t bit = 1u << pin;
    mmio_write32(GPREN0, mmio_read32(GPREN0) & ~bit);
    mmio_write32(GPFEN0, mmio_read32(GPFEN0) & ~bit);
    mmio_write32(GPEDS0, bit);
}
