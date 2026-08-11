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
#include "mmu.h"        // dcache_clean_inval (flush ring/CB to RAM for the DMA)

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

// ===========================================================================
// Streamed PCM audio (for games): PWM1 in balanced mode fed continuously from a
// ring buffer by a DMA channel, so the samples play with zero CPU involvement.
// Each PWM output cycle emits one sample as a duty ratio over RANGE clocks; the
// jack's analogue low-pass filter turns the pulse train back into audio. The
// FIFO is only 16 words deep, far too small to keep fed from a 35 Hz game loop,
// so a looping DMA does it. Real hardware only (QEMU models no PWM audio).
// ===========================================================================

// Extra PWM registers for FIFO/DMA streaming.
#define PWM_DMAC   (*(volatile uint32_t *)(PWM1_BASE + 0x08))
#define PWM_FIF1_BUS  0x7E20C818u          // FIFO, as the DMA (bus) addresses it
#define PWM_USEF1  (1u << 5)               // channel 1 reads from the FIFO
#define PWM_USEF2  (1u << 13)              // channel 2 reads from the FIFO
#define PWM_DMAC_EN (1u << 31)

// Clock-manager MASH for a fractional divisor (needed to hit the sample rate).
#define CM_MASH1   (1u << 9)

// Legacy DMA controller (channels 0..6). One channel drives the audio.
#define DMA_BASE   0xFE007000UL
#define DMA_CH     5                       // DMA channel used for audio
#define DMA_REG(o) (*(volatile uint32_t *)(DMA_BASE + DMA_CH * 0x100 + (o)))
#define DMA_CS         DMA_REG(0x00)
#define DMA_CONBLK_AD  DMA_REG(0x04)
#define DMA_SOURCE_AD  DMA_REG(0x0C)
#define DMA_ENABLE (*(volatile uint32_t *)(DMA_BASE + 0xFF0))
#define DMA_CS_ACTIVE  (1u << 0)
#define DMA_CS_RESET   (1u << 31)
// Transfer-info bits.
#define TI_WAIT_RESP   (1u << 3)
#define TI_DEST_DREQ   (1u << 6)
#define TI_SRC_INC     (1u << 8)
#define TI_PERMAP(d)   ((uint32_t)(d) << 16)
#define DREQ_PWM       5                   // PWM peripheral DREQ line

// SDRAM as the legacy DMA addresses it: the uncached 0xC0000000 alias (our
// buffers live in the low 1 GB, so this is valid). We flush the CPU's writes
// with dcache_clean_inval so the DMA reads what we wrote.
#define BUS(p)     ((uint32_t)(uintptr_t)(p) | 0xC0000000u)

// A DMA control block (32-byte aligned, read by the controller).
typedef struct __attribute__((aligned(32))) {
    uint32_t ti, source_ad, dest_ad, txfr_len, stride, nextconbk, pad0, pad1;
} dma_cb_t;

#define PCM_RANGE   1024                   // 10-bit samples (0..1023, mid = 512)
#define RING_FRAMES 4096                   // stereo frames in the ring (~0.37 s @ 11 kHz)

static dma_cb_t  g_cb __attribute__((aligned(32)));
static uint32_t  g_ring[RING_FRAMES * 2];  // interleaved L,R PWM samples
static uint32_t  g_write;                  // next frame index we will fill
static int       g_pcm_open;
static int       g_pcm_rate;

// Program the PWM clock generator to `hz` from the 54 MHz crystal, with a
// fractional (MASH) divisor so non-integer rates are hit closely.
static void set_pwm_clock(uint32_t hz) {
    uint32_t divi = OSC_HZ / hz;
    uint32_t divf = (uint32_t)(((uint64_t)(OSC_HZ % hz) << 12) / hz);
    if (divi < 2) divi = 2;
    CM_PWMCTL = CM_PASSWD | (CM_PWMCTL & ~CM_CTL_ENAB);
    while (CM_PWMCTL & CM_CTL_BUSY) { }
    CM_PWMDIV = CM_PASSWD | (divi << 12) | (divf & 0xFFF);
    CM_PWMCTL = CM_PASSWD | CM_SRC_OSC | CM_MASH1;
    CM_PWMCTL = CM_PASSWD | CM_SRC_OSC | CM_MASH1 | CM_CTL_ENAB;
    delay(150);
}

int snd_pcm_open(int rate) {
    g_pcm_rate = rate;
    g_write = 0;
    if (!g_real_hw) { g_pcm_open = 1; return 0; }   // silent (discard) on QEMU
    if (rate < 4000)  rate = 4000;
    if (rate > 48000) rate = 48000;

    // PWM base clock so one sample lasts RANGE clocks: clock = RANGE * rate.
    set_pwm_clock((uint32_t)PCM_RANGE * (uint32_t)rate);

    // Fill the ring with silence (mid-scale) and flush it to RAM for the DMA.
    for (int i = 0; i < RING_FRAMES * 2; i++) g_ring[i] = PCM_RANGE / 2;
    dcache_clean_inval(g_ring, sizeof g_ring);

    // PWM1: both channels FIFO-fed, balanced (not mark/space), DMA requests on.
    PWM_CTL  = PWM_CLRF1;
    delay(150);
    PWM_RNG1 = PCM_RANGE; PWM_RNG2 = PCM_RANGE;
    PWM_DMAC = PWM_DMAC_EN | (7u << 8) | 7u;         // enable, panic=7, dreq=7
    PWM_CTL  = PWM_PWEN1 | PWM_PWEN2 | PWM_USEF1 | PWM_USEF2;

    // DMA control block: stream the ring to the FIFO, looping forever.
    g_cb.ti        = TI_WAIT_RESP | TI_DEST_DREQ | TI_SRC_INC | TI_PERMAP(DREQ_PWM);
    g_cb.source_ad = BUS(g_ring);
    g_cb.dest_ad   = PWM_FIF1_BUS;
    g_cb.txfr_len  = sizeof g_ring;                  // bytes
    g_cb.stride    = 0;
    g_cb.nextconbk = BUS(&g_cb);                     // loop back to itself
    g_cb.pad0 = g_cb.pad1 = 0;
    dcache_clean_inval(&g_cb, sizeof g_cb);

    // Start the DMA channel.
    DMA_ENABLE |= (1u << DMA_CH);
    DMA_CS = DMA_CS_RESET;
    delay(150);
    DMA_CONBLK_AD = BUS(&g_cb);
    DMA_CS = DMA_CS_ACTIVE;
    g_pcm_open = 1;
    return 0;
}

// Free stereo frames the producer may fill without overtaking the DMA read head.
int snd_pcm_avail(void) {
    if (!g_pcm_open) return 0;
    if (!g_real_hw) return RING_FRAMES;              // discard everything on QEMU
    uint32_t read_off = DMA_SOURCE_AD - BUS(g_ring); // bytes the DMA is at
    uint32_t read_frame = (read_off / 4) / 2;        // -> frame index
    if (read_frame >= RING_FRAMES) read_frame = 0;
    int used = ((int)g_write - (int)read_frame + RING_FRAMES) % RING_FRAMES;
    int freef = RING_FRAMES - used - 1;              // -1: never fully catch up
    return freef < 0 ? 0 : freef;
}

// Write up to `frames` stereo 16-bit sample pairs; returns how many were taken.
int snd_pcm_write(const short *stereo, int frames) {
    if (!g_pcm_open || !g_real_hw) return frames;    // discard (silent) on QEMU
    int avail = snd_pcm_avail();
    if (frames > avail) frames = avail;
    for (int i = 0; i < frames; i++) {
        int l = (stereo[i * 2]     >> 6) + PCM_RANGE / 2;   // 16-bit signed -> 0..1023
        int r = (stereo[i * 2 + 1] >> 6) + PCM_RANGE / 2;
        if (l < 0) l = 0; else if (l > PCM_RANGE - 1) l = PCM_RANGE - 1;
        if (r < 0) r = 0; else if (r > PCM_RANGE - 1) r = PCM_RANGE - 1;
        g_ring[g_write * 2]     = (uint32_t)l;
        g_ring[g_write * 2 + 1] = (uint32_t)r;
        g_write = (g_write + 1) % RING_FRAMES;
    }
    dcache_clean_inval(g_ring, sizeof g_ring);        // flush for the DMA
    return frames;
}

void snd_pcm_close(void) {
    if (g_pcm_open && g_real_hw) {
        DMA_CS = DMA_CS_RESET;                         // stop the DMA
        PWM_CTL = 0;
        snd_init();                                   // restore tone-mode clock + idle
    }
    g_pcm_open = 0;
}
