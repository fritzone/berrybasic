// pinblink.sed - drive a header pin from native code, using the seed GPIO API.
// Shows a seed reaching the Raspberry Pi 4's 40-pin header through the services
// vtable (ABI v4): check availability, set a pin to output, and toggle it.
//
//   SEED h%, "PINBLINK.SED"
//   n% = CALL(h%, 17, 10)        : REM pin 17, 10 blinks, ~0.25 s each half
//   n% = CALL(h%, 17, 10, 5)     : REM ... or give the half-period in centiseconds
//
// Returns the number of on/off cycles performed, or 0 when there is no header
// (for example on the host build, where svc->gpio_avail() is 0).
#include "seed.h"

SEED_EXPORT(pinblink)
{
    if (argc < 2) return 0;
    if (!svc->gpio_avail()) return 0;                 // no GPIO on this build
    int pin    = (int)argv[0].num;
    int cycles = (int)argv[1].num;
    int half   = argc >= 3 ? (int)argv[2].num : 25;   // centiseconds high, then low

    svc->gpio_mode(pin, SEED_GPIO_OUT, 0);
    for (int i = 0; i < cycles; i++) {
        svc->gpio_write(pin, 1);
        uint32_t t = svc->time_cs(); while (svc->time_cs() - t < (uint32_t)half) { }
        svc->gpio_write(pin, 0);
        t = svc->time_cs(); while (svc->time_cs() - t < (uint32_t)half) { }
    }
    svc->gpio_write(pin, 0);                           // leave it low
    return (double)cycles;
}
