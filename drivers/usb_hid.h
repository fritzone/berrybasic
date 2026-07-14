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

// Map a HID usage code + modifier byte to a character (0 if none), honouring the
// active keyboard layout (see hid_set_layout) and AltGr (modifier bit 0x40).
char hid_to_ascii(uint8_t kc, uint8_t mod);

// Keyboard layout selection. Codes are two letters, case-insensitive:
//   "US" United States · "UK" United Kingdom · "NO" Norwegian · "DK" Danish
//   "SE" Swedish · "DE" German.
// hid_set_layout returns 1 on success, 0 if the code is unknown (layout kept).
// hid_layout_code returns the active code (never NULL); defaults to "US".
int         hid_set_layout(const char *code);
const char *hid_layout_code(void);

// Given a fresh 8-byte boot report and the caller's previous-report buffer,
// return the ASCII for the first newly-pressed key (0 if none). Updates prev[].
char hid_report_char(const uint8_t report[8], uint8_t prev[8]);

// Decode a HID boot-protocol mouse report (3 or 4 bytes):
//   byte0 = buttons  (bit0=left, bit1=right, bit2=middle)
//   byte1 = dX       (signed, +right)
//   byte2 = dY       (signed, +down)
//   byte3 = wheel    (signed, optional; 0 if len < 4)
// Fills *btn/*dx/*dy/*wheel. Returns 1 if the report was long enough to decode.
int hid_mouse_decode(const uint8_t *report, int len,
                     int *btn, int *dx, int *dy, int *wheel);

#endif
