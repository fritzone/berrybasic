#ifndef USB_KBD_H
#define USB_KBD_H

// Returns 1 if a keyboard was found and configured, 0 otherwise. Also
// enumerates a mouse on the same DWC2 port/hub if one is present (see
// usb_mouse_present).
int  usb_kbd_init(void);

// Poll keyboard. Returns ASCII character or 0 if nothing pressed.
int  usb_kbd_getchar(void);

// 1 if a HID mouse was found and configured during usb_kbd_init.
int  usb_mouse_present(void);

// Poll the mouse's interrupt endpoint once. If a fresh report arrived, fills the
// relative deltas / button bitmask (bit0=left,1=right,2=middle) and returns 1;
// otherwise returns 0 and leaves the outputs untouched.
int  usb_mouse_poll(int *btn, int *dx, int *dy, int *wheel);

#endif
