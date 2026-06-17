#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>

// Minimal xHCI host-controller driver for the VL805 (Pi 4 USB-A ports). Brings
// up one HID boot-protocol keyboard. Returns 1 if a keyboard was configured.
int  xhci_kbd_init(uintptr_t mmio_base);

// Poll the keyboard; returns an ASCII char or 0 if nothing newly pressed.
char xhci_kbd_getchar(void);

#endif
