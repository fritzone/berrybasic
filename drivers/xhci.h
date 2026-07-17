#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>

// Minimal xHCI host-controller driver for the VL805 (Pi 4 USB-A ports). Brings
// up one HID boot-protocol keyboard. Returns 1 if a keyboard was configured.
int  xhci_kbd_init(uintptr_t mmio_base);

// Poll the keyboard; returns an ASCII char or 0 if nothing newly pressed.
int  xhci_kbd_getchar(void);

// 1 if a HID mouse was enumerated (on another port of the onboard hub) during
// xhci_kbd_init.
int  xhci_mouse_present(void);

// Poll the mouse's interrupt endpoint once. On a fresh report, fills the
// relative deltas / button bitmask (bit0=left,1=right,2=middle) and returns 1;
// otherwise 0 with outputs untouched.
int  xhci_mouse_poll(int *btn, int *dx, int *dy, int *wheel);

#endif
