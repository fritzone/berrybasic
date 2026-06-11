#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
char uart_getc(void);        // returns 0 if RX FIFO empty
void uart_hex(const char *label, uint32_t v);
void uart_hex64(const char *label, uint64_t v);
void uart_dec(const char *label, uint32_t v);

// Return the in-RAM copy of everything written to the UART so far (for dumping
// the boot log to a file on the SD card).
void uart_log_get(const char **buf, int *len);

#endif
