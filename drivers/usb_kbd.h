#ifndef USB_KBD_H
#define USB_KBD_H

// Returns 1 if a keyboard was found and configured, 0 otherwise.
int  usb_kbd_init(void);

// Poll keyboard. Returns ASCII character or 0 if nothing pressed.
char usb_kbd_getchar(void);

#endif
