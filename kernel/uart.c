#include "uart.h"

#define PERIPHERAL_BASE 0xFE000000UL
#define GPIO_BASE       (PERIPHERAL_BASE + 0x200000)
#define UART0_BASE      (PERIPHERAL_BASE + 0x201000)

// GPIO registers
#define GPFSEL1         (*(volatile uint32_t *)(GPIO_BASE + 0x04))
#define GPPUPPDN0       (*(volatile uint32_t *)(GPIO_BASE + 0xE4))

// UART0 (PL011) registers
#define UART0_DR        (*(volatile uint32_t *)(UART0_BASE + 0x00))
#define UART0_FR        (*(volatile uint32_t *)(UART0_BASE + 0x18))
#define UART0_IBRD      (*(volatile uint32_t *)(UART0_BASE + 0x24))
#define UART0_FBRD      (*(volatile uint32_t *)(UART0_BASE + 0x28))
#define UART0_LCRH      (*(volatile uint32_t *)(UART0_BASE + 0x2C))
#define UART0_CR        (*(volatile uint32_t *)(UART0_BASE + 0x30))
#define UART0_IMSC      (*(volatile uint32_t *)(UART0_BASE + 0x38))
#define UART0_ICR       (*(volatile uint32_t *)(UART0_BASE + 0x44))

static void delay(uint32_t n) {
    for (volatile uint32_t i = 0; i < n; i++) __asm__ volatile("nop");
}

// In-RAM copy of everything sent to the UART, so the boot log can be dumped to a
// file on the SD card (handy when no serial cable is available).
#define LOG_CAP 32768
static char log_buf[LOG_CAP];
static int  log_len = 0;

void uart_log_get(const char **buf, int *len) {
    *buf = log_buf;
    *len = log_len;
}

void uart_init(void) {
    // Disable UART
    UART0_CR = 0;

    // Configure GPIO 14 (TXD0) and 15 (RXD0) as ALT0
    uint32_t r = GPFSEL1;
    r &= ~((7u << 12) | (7u << 15));   // clear pins 14 and 15 fields
    r |=  ((4u << 12) | (4u << 15));   // ALT0 = 0b100
    GPFSEL1 = r;

    // No pull-up/down on pins 14 and 15 (Pi 4 uses GPPUPPDN registers)
    r = GPPUPPDN0;
    r &= ~((3u << 28) | (3u << 30));   // clear bits for pins 14 and 15
    GPPUPPDN0 = r;

    delay(150);

    // Clear all pending interrupts
    UART0_ICR = 0x7FF;

    // 115200 baud at 48 MHz: IBRD=26, FBRD=3
    UART0_IBRD = 26;
    UART0_FBRD = 3;

    // 8 bits, no parity, 1 stop bit, FIFOs enabled
    UART0_LCRH = (3 << 5) | (1 << 4);

    // Mask all interrupts
    UART0_IMSC = 0;

    // Enable UART, TX, RX
    UART0_CR = (1 << 9) | (1 << 8) | (1 << 0);
}

void uart_putc(char c) {
    if (log_len < LOG_CAP) log_buf[log_len++] = c;   // tee into the RAM log
    while (UART0_FR & (1 << 5));   // wait while TX FIFO full
    UART0_DR = (uint32_t)c;
}

void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

char uart_getc(void) {
    if (UART0_FR & (1 << 4)) return 0;   // RX FIFO empty
    char c = (char)(UART0_DR & 0xFF);
    if (c == 0x7F) c = '\b';             // DEL → backspace
    return c;
}

void uart_hex(const char *label, uint32_t v) {
    uart_puts(label);
    const char *h = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) uart_putc(h[(v >> i) & 0xF]);
    uart_putc('\n');
}

void uart_hex64(const char *label, uint64_t v) {
    uart_puts(label);
    const char *h = "0123456789ABCDEF";
    for (int i = 60; i >= 0; i -= 4) uart_putc(h[(v >> i) & 0xF]);
    uart_putc('\n');
}

void uart_dec(const char *label, uint32_t v) {
    uart_puts(label);
    if (v == 0) { uart_puts("0\n"); return; }
    char buf[12]; int n = 0;
    while (v) { buf[n++] = '0' + (v % 10); v /= 10; }
    for (int i = n - 1; i >= 0; i--) uart_putc(buf[i]);
    uart_putc('\n');
}
