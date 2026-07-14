#ifndef I2C_H
#define I2C_H
#include <stdint.h>

// I2C master on the Raspberry Pi 4's primary bus (BSC1): SDA1 = BCM 2 (header
// pin 3), SCL1 = BCM 3 (header pin 5). Two backends implement this: the bare-
// metal BCM2711 BSC driver (drivers/i2c.c) and a host stub (host/i2c_host.c).
//
// Real-hardware only. Unlike plain GPIO, QEMU's raspi4b does NOT model the BSC
// peripheral, so i2c_available() returns 0 under the emulator (and on the host);
// the interpreter checks it and raises "I2C needs real Pi hardware" rather than
// touching an unmodelled peripheral (which would hang). Addresses are 7-bit.

int  i2c_available(void);            // 1 only on real Pi hardware, else 0

// Release the bus (disable the controller); the next call re-initialises it.
// Called at RUN/NEW alongside gpio_reset(), since that returns SDA/SCL to inputs.
void i2c_reset(void);

// Write n bytes from buf / read n bytes into buf, to/from the device at `addr`.
// Each is one complete transaction (start ... stop). Returns 0 on success, or
// <0 on a NACK, a clock-stretch timeout, or the safety timeout (never hangs).
int  i2c_write(int addr, const unsigned char *buf, int n);
int  i2c_read (int addr, unsigned char *buf, int n);

// Probe an address: 1 if a device acknowledges, 0 if not (used for bus scans).
int  i2c_probe(int addr);

#endif
