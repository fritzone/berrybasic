// Host stub for the I2C API: there is no hardware bus when the interpreter runs
// on Linux, so the bus is reported unavailable and every operation fails. The
// interpreter checks i2c_available() and raises "I2C needs real Pi hardware".
#include "i2c.h"

int  i2c_available(void) { return 0; }
void i2c_reset(void) {}
int  i2c_write(int addr, const unsigned char *buf, int n) { (void)addr; (void)buf; (void)n; return -1; }
int  i2c_read (int addr, unsigned char *buf, int n)       { (void)addr; (void)buf; (void)n; return -1; }
int  i2c_probe(int addr) { (void)addr; return 0; }
