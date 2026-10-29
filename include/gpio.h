#ifndef GPIO_H
#define GPIO_H
#include <stdint.h>

// Digital GPIO abstraction that the BASIC interpreter talks to. Two backends
// implement it: the bare-metal BCM2711 driver on the Raspberry Pi 4's 40-pin
// header (drivers/gpio.c) and a host stub (host/gpio_host.c) used when testing
// the interpreter on Linux, where there is no hardware to poke.
//
// Numbering is BCM throughout: pin 17 means BCM GPIO 17, not header pin 17.
// Only BCM 0..27 are reachable (the 40-pin header); the interpreter validates
// the range and the driver never touches pins above that.

// Pin function modes (interface-level; gpio.c maps to raw FSEL bits).
enum { GPIO_IN = 0, GPIO_OUT = 1, GPIO_ALT = 2 };
// Pull settings.
enum { GPIO_PULL_NONE = 0, GPIO_PULL_UP = 1, GPIO_PULL_DOWN = 2 };

// 1 on the Pi/QEMU, 0 on the host build. The interpreter checks this and raises
// "GPIO needs the Pi, not the host build" when 0.
int  gpio_available(void);

// Reset every header pin (BCM 0..27) to input, no pull. Called at RUN/NEW/LOAD/boot.
void gpio_reset(void);

// Configure one pin. `mode` is GPIO_IN/GPIO_OUT/GPIO_ALT; for GPIO_ALT, `alt`
// selects function 0..5 (ignored otherwise). Returns 0 ok, <0 on a bad pin/alt.
int  gpio_set_mode(int pin, int mode, int alt);

// Set the internal pull resistor on a pin (GPIO_PULL_*). Returns 0 ok, <0 bad pin.
int  gpio_set_pull(int pin, int pull);

// Single-pin write/read. `level` is 0/1; gpio_read returns 0/1.
void gpio_write(int pin, int level);
int  gpio_read(int pin);

// Whole-port atomic set/clear (low 28 bits) and bulk read.
void     gpio_set_mask(uint32_t mask);   // GPSET0
void     gpio_clr_mask(uint32_t mask);   // GPCLR0
uint32_t gpio_read_all(void);            // GPLEV0

// Wait up to `timeout_cs` centiseconds (1/100 s) for an edge on `pin`. `edge` is
// 0 = falling, 1 = rising. Returns `pin` when the edge arrives, or -1 on timeout
// (shape mirrors the INKEY reader). A timeout_cs <= 0 polls once and returns
// immediately. The host stub always returns -1.
int gpio_wait_edge(int pin, int edge, int timeout_cs);

// Non-blocking edge detection for the BASIC event system (ON PIN). Arm a pin's
// rising/falling/both detector, poll (and clear) its latch, or disarm it.
// edge: 0 = falling, 1 = rising, 2 = both.
void gpio_arm_edge(int pin, int edge);
int  gpio_poll_edge(int pin);      // 1 if an edge was latched (clears it), else 0
void gpio_disarm_edge(int pin);

#endif
