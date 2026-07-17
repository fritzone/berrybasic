#ifndef USB_HID_H
#define USB_HID_H

#include <stdint.h>

// Shared USB HID boot-keyboard decoding, used by both the DWC2 driver (USB-C
// port, QEMU) and the xHCI/VL805 driver (USB-A ports, real Pi 4 hardware).

// A key is an int, not a char. A key that types something returns that
// character (1..255, Latin-1); a key that types nothing returns one of the
// codes below. The editing keys keep their historical control codes, because
// programs and the line editor already know them; everything added since sits
// above 0xFF so that the whole 8-bit range stays available for real text
// (æ ø å ä ö ü ß § £ € …), which is why the key path is `int`-wide throughout.
#define KEY_LEFT   0x11
#define KEY_RIGHT  0x12
#define KEY_UP     0x13
#define KEY_DOWN   0x14
#define KEY_HOME   0x15
#define KEY_END    0x16
#define KEY_INS    0x17   // Insert: toggles the editor's insert/overwrite mode
#define KEY_ESC    0x1B   // Escape: the real ASCII code
#define KEY_DEL    0x7F   // forward delete

#define KEY_F1     0x101  // F1..F12 are consecutive: use KEY_F(n)
#define KEY_F12    0x10C
#define KEY_F(n)   (KEY_F1 + (n) - 1)
#define KEY_PGUP   0x10D
#define KEY_PGDN   0x10E

// Modifier and lock state, as reported by hid_modifiers(). Left and right of a
// pair are folded together (a program that cares which is which wants a bigger
// API than this); AltGr is the right-hand Alt, which types the third legend on
// Nordic/German boards and is therefore worth telling apart from plain Alt.
#define KMOD_SHIFT  0x001
#define KMOD_CTRL   0x002
#define KMOD_ALT    0x004
#define KMOD_ALTGR  0x008
#define KMOD_META   0x010   // the Windows / Command key
#define KMOD_CAPS   0x020   // lock states, toggled by their keys
#define KMOD_NUM    0x040
#define KMOD_SCROLL 0x080

// Map a HID usage code + modifier byte to a key, honouring the active layout
// (see hid_set_layout), AltGr (modifier bit 0x40) and CapsLock. 0 = this key
// produces nothing at all.
int hid_to_key(uint8_t kc, uint8_t mod);

// The modifiers and locks held/set as of the last report seen by
// hid_report_key. NOTE this is a *snapshot*, refreshed only when the keyboard
// is actually read: poll a key (con_inkey(0) or the seed's inkey(0)) first, or
// the answer is however old your last read was.
int hid_modifiers(void);

// The keyboard's LED state, as the HID output report byte the device expects
// (bit0 NumLock, bit1 CapsLock, bit2 ScrollLock). hid_leds_dirty() reports that
// the locks changed and the device has not been told yet; the USB backends poll
// it after each report and SET_REPORT the new value, then ack.
int  hid_leds(void);
int  hid_leds_dirty(void);
void hid_leds_ack(void);

// Keyboard layout selection. Codes are two letters, case-insensitive:
//   "US" United States · "UK" United Kingdom · "NO" Norwegian · "DK" Danish
//   "SE" Swedish · "DE" German.
// hid_set_layout returns 1 on success, 0 if the code is unknown (layout kept).
// hid_layout_code returns the active code (never NULL); defaults to "US".
int         hid_set_layout(const char *code);
const char *hid_layout_code(void);

// Given a fresh 8-byte boot report and the caller's previous-report buffer,
// return the first newly-pressed key (0 if none). Also refreshes the modifier
// snapshot and applies the lock keys (which toggle a lock and produce no key).
// Updates prev[].
int hid_report_key(const uint8_t report[8], uint8_t prev[8]);

// Decode a HID boot-protocol mouse report (3 or 4 bytes):
//   byte0 = buttons  (bit0=left, bit1=right, bit2=middle)
//   byte1 = dX       (signed, +right)
//   byte2 = dY       (signed, +down)
//   byte3 = wheel    (signed, optional; 0 if len < 4)
// Fills *btn/*dx/*dy/*wheel. Returns 1 if the report was long enough to decode.
int hid_mouse_decode(const uint8_t *report, int len,
                     int *btn, int *dx, int *dy, int *wheel);

#endif
