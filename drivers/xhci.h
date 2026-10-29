#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>

// Minimal xHCI host-controller driver for the VL805 (Pi 4 USB-A ports). Brings
// up HID boot devices (keyboard/mouse) AND now enumerates every device into a
// table and offers a generic control + bulk transport, so a USB mass-storage
// stick can be driven on top (see msc.c). Returns 1 if a keyboard was configured.
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

// --- generic device table + transport (Phase 2) -----------------------------
// Every device found during xhci_kbd_init is recorded here. `cls/subcls/proto`
// come from the device's first INTERFACE descriptor (0x08/0x06/0x50 = a BOT/SCSI
// mass-storage stick). Enumeration order; `valid` is 0 for empty slots.
typedef struct xhci_dev {
    int slot, speed, root_port, tt_slot, tt_port;
    uint32_t route;
    uint8_t  cls, subcls, proto;      // from the INTERFACE descriptor
    int      ifnum, valid;
    int      bulk_in, bulk_out, bulk_mps;  // mass-storage bulk endpoints (0 if none)
    int      hub_port;                // downstream port of the onboard hub (0 = root)
} xhci_dev;

int       xhci_dev_count(void);
xhci_dev *xhci_dev_get(int i);        // 0 if out of range

// --- hot-plug (real hardware) -----------------------------------------------
// Re-poll the onboard hub's downstream ports: enumerate any newly-attached
// device into the device table, and mark+free any that vanished. Cheap when
// nothing changed (a few control transfers). Returns a bitmask: 1 = a device was
// added, 2 = a device was removed, 0 = no change. Call it periodically.
int  xhci_rescan(void);
// 1 if `slot` still describes a live (valid) enumerated device.
int  xhci_slot_valid(int slot);

// Control transfer on a device's default (EP0) pipe. Buffers MUST come from
// xhci_dma_alloc. Returns 0 on success, <0 on error.
int xhci_control(int slot, unsigned char bmReqType, unsigned char bReq,
                 unsigned short wValue, unsigned short wIndex,
                 unsigned short wLength, void *buf);

// Configure a device's bulk IN + OUT endpoints in one CONFIGURE_ENDPOINT.
// ep_in/ep_out are endpoint ADDRESSES (e.g. 0x81, 0x02); mps is the max packet
// size. 0 on success, <0 on error.
int xhci_bulk_config(int slot, int ep_in, int ep_out, int mps);

// Bulk transfers. Buffers MUST come from xhci_dma_alloc. -> bytes transferred,
// or <0: -1 timeout, -2 stall.
int xhci_bulk_in (int slot, void *buf, int len, int timeout_ms);
int xhci_bulk_out(int slot, const void *buf, int len, int timeout_ms);

// Clear a halted bulk endpoint (CLEAR_FEATURE(HALT) + RESET_ENDPOINT). ep_addr
// is the endpoint address (0x81 / 0x02). 0 on success, <0 on error.
int xhci_ep_reset(int slot, int ep_addr);

// Allocate zeroed, aligned memory from the controller's non-cached DMA arena.
// Bump-only (never freed): allocate once at init, never per transfer.
void *xhci_dma_alloc(unsigned size, unsigned align);

// Phase 2 bring-up self-test: log every enumerated device (class/subclass/proto,
// and a mass-storage device's bulk endpoints + max packet size) over the UART.
void xhci_selftest(void);

#endif
