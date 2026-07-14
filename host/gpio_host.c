// Host backend for gpio.h. The 40-pin header does not exist on a Linux test box,
// so this reports the hardware as unavailable and makes every operation a no-op
// (reads return 0). The interpreter checks gpio_available() at the start of each
// GPIO verb and raises "GPIO needs the Pi, not the host build", exactly like the
// seed guard. This keeps the shared basic.c linking and lets every non-GPIO
// program run unchanged under the host unit tests.

#include "gpio.h"

int  gpio_available(void) { return 0; }
void gpio_reset(void) { }
int  gpio_set_mode(int pin, int mode, int alt) { (void)pin; (void)mode; (void)alt; return 0; }
int  gpio_set_pull(int pin, int pull) { (void)pin; (void)pull; return 0; }
void gpio_write(int pin, int level) { (void)pin; (void)level; }
int  gpio_read(int pin) { (void)pin; return 0; }
void gpio_set_mask(uint32_t mask) { (void)mask; }
void gpio_clr_mask(uint32_t mask) { (void)mask; }
uint32_t gpio_read_all(void) { return 0; }
int gpio_wait_edge(int pin, int edge, int timeout_cs) {
    (void)pin; (void)edge; (void)timeout_cs; return -1;   // no header: always "timeout"
}
void gpio_arm_edge(int pin, int edge) { (void)pin; (void)edge; }
int  gpio_poll_edge(int pin) { (void)pin; return 0; }     // no header: never fires
void gpio_disarm_edge(int pin) { (void)pin; }
