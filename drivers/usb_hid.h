#ifndef USB_HID_H
#define USB_HID_H

#include <stdint.h>

// Shared USB HID boot-keyboard decoding, used by both the DWC2 driver (USB-C
// port, QEMU) and the xHCI/VL805 driver (USB-A ports, real Pi 4 hardware).

// Editing keys returned by the keyboard decode (and recognised by the line
// editor). They are otherwise-unused control codes so a program reading GET can
// still tell them apart.
#define KEY_LEFT   0x11
#define KEY_RIGHT  0x12
#define KEY_UP     0x13
#define KEY_DOWN   0x14
#define KEY_HOME   0x15
#define KEY_END    0x16
#define KEY_DEL    0x7F   // forward delete

// Map a HID usage code + modifier byte to an ASCII character (0 if none).
char hid_to_ascii(uint8_t kc, uint8_t mod);

// Given a fresh 8-byte boot report and the caller's previous-report buffer,
// return the ASCII for the first newly-pressed key (0 if none). Updates prev[].
char hid_report_char(const uint8_t report[8], uint8_t prev[8]);

#endif
