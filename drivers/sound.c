// Bare-metal audio for the Raspberry Pi 4 (BCM2711): a single square-wave voice
// on the 3.5mm analogue jack, driven by the PWM peripheral in mark/space mode.
//
// Why mark/space and not a sample stream: the interpreter only ever asks for one
// tone at a time (see sound.h and the sound_* engine in basic.c). In mark/space
// (MSEN) mode the PWM hardware emits a repeating "high for DATA cycles, low for
// RANGE-DATA cycles" pattern all by itself, so a tone sustains with zero CPU
// involvement until we retune or silence it. That is exactly what a background,
// queued player needs — no DMA, no interrupts.
//
// Output frequency = pwm_clock / RANGE. We run the PWM clock at a tidy 1 MHz, so
// RANGE = 1_000_000 / freq and a 50%-ish duty (DATA = RANGE/2) is the loudest
// square wave; a narrower pulse is quieter, which is how `vol` is implemented.
//
// Pin/route facts for the Pi 4 (differ from earlier Pis): the jack is GPIO40 and
// GPIO41 in ALT0, which are PWM1_0 (left) and PWM1_1 (right); PWM1 is at
// 0x7e20c800 (bus) = 0xFE20C800 (ARM). The BCM2711 crystal is 54 MHz.
//
// NOTE: QEMU's raspi4b does not emulate PWM audio to a host sound device, so
// this only produces sound on real hardware. Under QEMU it is a silent no-op
// (the register writes go to unmodelled MMIO).

#include <stdint.h>
#include "sound.h"

#define PERIPHERAL_BASE 0xFE000000UL
#define GPIO_BASE       (PERIPHERAL_BASE + 0x200000)

// GPIO function-select for pins 40..49 (three bits per pin) and the Pi 4
// pull-up/down control for pins 32..47.
#define GPFSEL4    (*(volatile uint32_t *)(GPIO_BASE + 0x10))
#define GPPUPPDN2  (*(volatile uint32_t *)(GPIO_BASE + 0xEC))

// Clock manager: the PWM clock generator (password-protected).
#define CM_PWMCTL  (*(volatile uint32_t *)(PERIPHERAL_BASE + 0x1010A0))
#define CM_PWMDIV  (*(volatile uint32_t *)(PERIPHERAL_BASE + 0x1010A4))
#define CM_PASSWD       0x5A000000u
#define CM_CTL_ENAB     (1u << 4)
#define CM_CTL_BUSY     (1u << 7)
#define CM_SRC_OSC      1u             // clock source 1 = crystal oscillator

// PWM1 controller (the one wired to the Pi 4 jack).
#define PWM1_BASE  (PERIPHERAL_BASE + 0x20C800)
#define PWM_CTL    (*(volatile uint32_t *)(PWM1_BASE + 0x00))
#define PWM_STA    (*(volatile uint32_t *)(PWM1_BASE + 0x04))
#define PWM_RNG1   (*(volatile uint32_t *)(PWM1_BASE + 0x10))
#define PWM_DAT1   (*(volatile uint32_t *)(PWM1_BASE + 0x14))
#define PWM_RNG2   (*(volatile uint32_t *)(PWM1_BASE + 0x20))
#define PWM_DAT2   (*(volatile uint32_t *)(PWM1_BASE + 0x24))

// PWM_CTL bits (channel 1 = GPIO40/left, channel 2 = GPIO41/right).
#define PWM_PWEN1  (1u << 0)
#define PWM_MSEN1  (1u << 7)
#define PWM_PWEN2  (1u << 8)
#define PWM_MSEN2  (1u << 15)
#define PWM_CLRF1  (1u << 6)

#define OSC_HZ      54000000u          // BCM2711 crystal
#define PWM_CLK_HZ  1000000u           // PWM base clock we run at (1 MHz)

static void delay(uint32_t n) {
    for (volatile uint32_t i = 0; i < n; i++) __asm__ volatile("nop");
}

// Set at boot from the board serial (see kernel.c). QEMU's raspi4b does not model
// the PWM peripheral, so touching these registers there raises an external abort;
// on QEMU this stays 0 and every entry point below becomes a no-op.
extern int g_real_hw;

void snd_init(void) {
    if (!g_real_hw) return;
    // Route GPIO40 and GPIO41 to ALT0 (PWM1_0 / PWM1_1). In GPFSEL4 pin 40 is
    // field 0 (bits 0..2) and pin 41 is field 1 (bits 3..5); ALT0 = 0b100.
    uint32_t r = GPFSEL4;
    r &= ~((7u << 0) | (7u << 3));
    r |=  ((4u << 0) | (4u << 3));
    GPFSEL4 = r;

    // No pull-up/down on the audio pins. In GPPUPPDN2 (pins 32..47) pin 40 is
    // field 8 (bits 16..17) and pin 41 is field 9 (bits 18..19); 0b00 = none.
    r = GPPUPPDN2;
    r &= ~((3u << 16) | (3u << 18));
    GPPUPPDN2 = r;

    // Program the PWM clock to PWM_CLK_HZ from the crystal oscillator. Stop it,
    // wait for BUSY to clear, set an integer-only divisor, then re-enable.
    CM_PWMCTL = CM_PASSWD | (CM_PWMCTL & ~CM_CTL_ENAB);
    while (CM_PWMCTL & CM_CTL_BUSY) { }
    CM_PWMDIV = CM_PASSWD | ((OSC_HZ / PWM_CLK_HZ) << 12);   // DIVI, no fractional part
    CM_PWMCTL = CM_PASSWD | CM_SRC_OSC;                      // select source (still stopped)
    CM_PWMCTL = CM_PASSWD | CM_SRC_OSC | CM_CTL_ENAB;        // and go
    delay(150);

    PWM_CTL = PWM_CLRF1;    // clear FIFO / reset the controller
    delay(150);
    PWM_CTL = 0;            // both channels off = idle low = silent
}

void snd_silence(void) {
    if (!g_real_hw) return;
    PWM_CTL = 0;
}

void snd_set_tone(int freq_hz, int vol) {
    if (!g_real_hw) return;
    if (freq_hz <= 0 || vol <= 0) { snd_silence(); return; }
    if (freq_hz < 20)    freq_hz = 20;         // keep RANGE within sane bounds
    if (freq_hz > 20000) freq_hz = 20000;
    if (vol > 15) vol = 15;

    uint32_t range = PWM_CLK_HZ / (uint32_t)freq_hz;   // clocks per output period
    if (range < 2) range = 2;

    // Duty cycle carries loudness: vol 15 -> 50% (the loudest square wave),
    // smaller vol -> a narrower pulse that sounds quieter.
    uint32_t data = (range * (uint32_t)vol) / 30;
    if (data < 1) data = 1;
    if (data >= range) data = range - 1;

    PWM_RNG1 = range; PWM_DAT1 = data;    // left  (GPIO40)
    PWM_RNG2 = range; PWM_DAT2 = data;    // right (GPIO41)
    PWM_CTL  = PWM_PWEN1 | PWM_MSEN1 | PWM_PWEN2 | PWM_MSEN2;
}
