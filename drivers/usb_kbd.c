#include <stdint.h>
#include "usb_kbd.h"
#include "usb_hid.h"
#include "uart.h"
#include "mailbox.h"

// ---------------------------------------------------------------------------
// DWC2 USB OTG host-mode driver  (Raspberry Pi 4, BCM2711)
// Handles a USB hub + HID boot-protocol keyboard.
// QEMU raspi4b puts a virtual hub between DWC2 root port and devices, so we
// enumerate the hub, then the keyboard behind it.
// ---------------------------------------------------------------------------

#define DWC2_BASE   0xFE980000UL

#define GOTGCTL     (*(volatile uint32_t *)(DWC2_BASE + 0x000))
#define GAHBCFG     (*(volatile uint32_t *)(DWC2_BASE + 0x008))
#define GUSBCFG     (*(volatile uint32_t *)(DWC2_BASE + 0x00C))
#define GRSTCTL     (*(volatile uint32_t *)(DWC2_BASE + 0x010))
#define GINTSTS     (*(volatile uint32_t *)(DWC2_BASE + 0x014))
#define GINTMSK     (*(volatile uint32_t *)(DWC2_BASE + 0x018))
#define GRXFSIZ     (*(volatile uint32_t *)(DWC2_BASE + 0x024))
#define GNPTXFSIZ   (*(volatile uint32_t *)(DWC2_BASE + 0x028))
#define HPTXFSIZ    (*(volatile uint32_t *)(DWC2_BASE + 0x100))
#define HCFG        (*(volatile uint32_t *)(DWC2_BASE + 0x400))
#define HPRT        (*(volatile uint32_t *)(DWC2_BASE + 0x440))

#define HCCHAR(n)   (*(volatile uint32_t *)(DWC2_BASE + 0x500 + (n)*0x20))
#define HCSPLT(n)   (*(volatile uint32_t *)(DWC2_BASE + 0x504 + (n)*0x20))
#define HCINT(n)    (*(volatile uint32_t *)(DWC2_BASE + 0x508 + (n)*0x20))
#define HCINTMSK(n) (*(volatile uint32_t *)(DWC2_BASE + 0x50C + (n)*0x20))
#define HCTSIZ(n)   (*(volatile uint32_t *)(DWC2_BASE + 0x510 + (n)*0x20))
#define HCDMA(n)    (*(volatile uint32_t *)(DWC2_BASE + 0x514 + (n)*0x20))
#define PCGCR       (*(volatile uint32_t *)(DWC2_BASE + 0xE00))   // power/clock gating

#define HCINT_XFERCOMPL  (1u <<  0)
#define HCINT_CHHLTD     (1u <<  1)
#define HCINT_STALL      (1u <<  3)
#define HCINT_NAK        (1u <<  4)
#define HCINT_XACTERR    (1u <<  7)

#define EP_CTRL  0
#define EP_INTR  3

#define PID_DATA0  0
#define PID_DATA1  2
#define PID_SETUP  3

#define HPRT_W1C  ((1u<<1)|(1u<<3)|(1u<<5))

#define TIMER_CLO  (*(volatile uint32_t *)(0xFE003004UL))

static void usleep(uint32_t us) {
    uint32_t t0 = TIMER_CLO;
    while (TIMER_CLO - t0 < us);
}

// ---------------------------------------------------------------------------
// HPRT helpers
// ---------------------------------------------------------------------------

static void hprt_set(uint32_t bit) {
    HPRT = (HPRT & ~HPRT_W1C) | bit;
}
static void hprt_clr(uint32_t bit) {
    HPRT = (HPRT & ~HPRT_W1C) & ~bit;
}

// ---------------------------------------------------------------------------
// DWC2 core reset / flush
// ---------------------------------------------------------------------------

// On real Pi 4 hardware the firmware may leave the DWC2 OTG controller powered
// down with its PHY clock gated, so the core soft reset never completes (it
// works in QEMU because the clock is always running). Ask the firmware to power
// the on-SoC USB controller on, then clear the power/clock-gating register.
static void dwc2_power_on(void) {
    mbox[0] = 8 * 4; mbox[1] = 0;
    mbox[2] = 0x00028001;            // SET_POWER_STATE
    mbox[3] = 8; mbox[4] = 8;
    mbox[5] = 3;                     // device id 3 = USB HCD
    mbox[6] = 3;                     // bit0 = on, bit1 = wait until stable
    mbox[7] = 0;                     // end tag
    mbox_call();
    uart_hex("[DWC2] usb power state: ", mbox[6]);
    usleep(20000);
    PCGCR = 0;                       // ungate the PHY/AHB clock
    usleep(10000);
}

static int dwc2_reset(void) {
    for (uint32_t t = 0; !(GRSTCTL & (1u<<31)); t++)
        if (t > 1000000) { uart_puts("[DWC2] AHB never idle\n"); return -1; }
    GRSTCTL |= 1u;
    for (uint32_t t = 0; GRSTCTL & 1u; t++)
        if (t > 1000000) { uart_puts("[DWC2] reset timeout\n"); return -2; }
    usleep(3);
    return 0;
}

static void dwc2_flush_tx(void) {
    GRSTCTL = (0x10u << 6) | (1u << 5);
    for (uint32_t t = 0; GRSTCTL & (1u<<5); t++) if (t > 1000000) break;
}
static void dwc2_flush_rx(void) {
    GRSTCTL = (1u << 4);
    for (uint32_t t = 0; GRSTCTL & (1u<<4); t++) if (t > 1000000) break;
}

// ---------------------------------------------------------------------------
// DMA buffers
// ---------------------------------------------------------------------------

static uint8_t setup_pkt[8]    __attribute__((aligned(4)));
static uint8_t ctrl_buf[256]   __attribute__((aligned(4)));
static uint8_t status_buf[4]   __attribute__((aligned(4)));
static uint8_t kbd_report[8]   __attribute__((aligned(4)));

// ---------------------------------------------------------------------------
// Single-channel transfer; returns 0=ok, -4=NAK, -2=STALL, -1=error
// ---------------------------------------------------------------------------

static int dwc2_xfer(int ch, int devaddr, int ep, int dir,
                     int type, int mps, int pid,
                     void *buf, int len, int lowspeed) {
    HCSPLT(ch)   = 0;
    HCINT(ch)    = 0xFFFFFFFFu;
    HCINTMSK(ch) = 0;
    HCDMA(ch)    = (uint32_t)(uintptr_t)(buf ? buf : status_buf);

    int pkts = (len > 0) ? ((len + mps - 1) / mps) : 1;
    HCTSIZ(ch) = ((uint32_t)pid << 29) | ((uint32_t)pkts << 19) | (uint32_t)len;

    HCCHAR(ch) = ((uint32_t)(devaddr & 0x7F) << 22) |
                 ((uint32_t)(ep & 0xF)       << 11) |
                 ((uint32_t)(dir & 1)         << 15) |
                 ((uint32_t)(type & 3)        << 18) |
                 (1u                           << 20) |  // MC=1
                 (lowspeed ? (1u << 17) : 0)         |
                 (uint32_t)(mps & 0x7FF)             |
                 (1u << 31);                            // ChEna

    for (uint32_t t = 0; ; t++) {
        uint32_t intr = HCINT(ch);
        if (intr & HCINT_CHHLTD) {
            HCINT(ch) = intr;
            if (intr & HCINT_XFERCOMPL) return  0;
            if (intr & HCINT_STALL)     return -2;
            if (intr & HCINT_NAK)       return -4;
            if (intr & HCINT_XACTERR)   return -3;
            return -1;
        }
        if (t > 2000000u) {
            HCCHAR(ch) |= (1u<<30)|(1u<<31);
            for (uint32_t w = 0; !(HCINT(ch) & HCINT_CHHLTD) && w < 1000000; w++);
            HCINT(ch) = 0xFFFFFFFFu;
            return -5;
        }
    }
}

static int dwc2_xfer_retry(int ch, int devaddr, int ep, int dir,
                            int type, int mps, int pid,
                            void *buf, int len, int lowspeed, int maxnak) {
    for (int n = 0; n < maxnak; n++) {
        int r = dwc2_xfer(ch, devaddr, ep, dir, type, mps, pid, buf, len, lowspeed);
        if (r != -4) return r;
        usleep(100);
    }
    return -4;
}

// ---------------------------------------------------------------------------
// USB control transfer
// ---------------------------------------------------------------------------

static int ctrl_xfer(int devaddr, int mps, int lowspeed,
                     uint8_t bmReqType, uint8_t bReq,
                     uint16_t wVal, uint16_t wIdx, uint16_t wLen,
                     void *data) {
    setup_pkt[0] = bmReqType; setup_pkt[1] = bReq;
    setup_pkt[2] = wVal & 0xFF; setup_pkt[3] = wVal >> 8;
    setup_pkt[4] = wIdx & 0xFF; setup_pkt[5] = wIdx >> 8;
    setup_pkt[6] = wLen & 0xFF; setup_pkt[7] = wLen >> 8;

    int r = dwc2_xfer_retry(0, devaddr, 0, 0, EP_CTRL, mps,
                             PID_SETUP, setup_pkt, 8, lowspeed, 50);
    if (r < 0) return r;

    if (wLen > 0) {
        int ddir = (bmReqType & 0x80) ? 1 : 0;
        r = dwc2_xfer_retry(0, devaddr, 0, ddir, EP_CTRL, mps,
                             PID_DATA1, data, wLen, lowspeed, 50);
        if (r < 0) return r;
    }

    int sdir = (bmReqType & 0x80) ? 0 : 1;
    dwc2_xfer_retry(0, devaddr, 0, sdir, EP_CTRL, mps,
                    PID_DATA1, status_buf, 0, lowspeed, 50);
    return 0;
}

// ---------------------------------------------------------------------------
// USB descriptor structures
// ---------------------------------------------------------------------------

typedef struct __attribute__((packed)) {
    uint8_t  bLength; uint8_t bDescriptorType;
    uint16_t bcdUSB; uint8_t bDeviceClass; uint8_t bDeviceSubClass;
    uint8_t  bDeviceProtocol; uint8_t bMaxPacketSize0;
    uint16_t idVendor; uint16_t idProduct; uint16_t bcdDevice;
    uint8_t  iManufacturer; uint8_t iProduct; uint8_t iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_dev_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength; uint8_t bDescriptorType;
    uint16_t wTotalLength; uint8_t bNumInterfaces;
    uint8_t bConfigurationValue; uint8_t iConfiguration;
    uint8_t bmAttributes; uint8_t bMaxPower;
} usb_cfg_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength; uint8_t bDescriptorType;
    uint8_t bEndpointAddress; uint8_t bmAttributes;
    uint16_t wMaxPacketSize; uint8_t bInterval;
} usb_ep_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t bDescLength; uint8_t bDescriptorType;
    uint8_t bNbrPorts; uint16_t wHubCharacteristics;
    uint8_t bPwrOn2PwrGood; uint8_t bHubContrCurrent;
} usb_hub_desc_t;

// ---------------------------------------------------------------------------
// Hub helpers
// ---------------------------------------------------------------------------

// Port status 32-bit layout: low 16 = wPortStatus, high 16 = wPortChange
#define PORT_STS_CONNECTION  (1u <<  0)
#define PORT_STS_ENABLE      (1u <<  1)
#define PORT_CHG_CONNECTION  (1u << 16)  // C_PORT_CONNECTION in wPortChange
#define PORT_CHG_RESET       (1u << 20)  // C_PORT_RESET in wPortChange

#define HUB_FEAT_PORT_RESET     4
#define HUB_FEAT_PORT_POWER     8
#define HUB_FEAT_C_CONNECTION  16
#define HUB_FEAT_C_RESET       20

static uint8_t hub_buf[16] __attribute__((aligned(4)));

static int hub_port_status(int addr, int mps, int port, uint32_t *st) {
    int r = ctrl_xfer(addr, mps, 0, 0xA3, 0, 0, port, 4, hub_buf);
    if (r == 0) *st = *(uint32_t *)hub_buf;
    return r;
}
static int hub_set_feat(int addr, int mps, int port, int feat) {
    return ctrl_xfer(addr, mps, 0, 0x23, 3, feat, port, 0, 0);
}
static int hub_clr_feat(int addr, int mps, int port, int feat) {
    return ctrl_xfer(addr, mps, 0, 0x23, 1, feat, port, 0, 0);
}

// ---------------------------------------------------------------------------
// Keyboard state
// ---------------------------------------------------------------------------

static int  kbd_devaddr  = 0;
static int  kbd_ep       = 0;
static int  kbd_mps      = 8;
static int  kbd_lowspeed = 0;
static int  kbd_pid      = PID_DATA0;
static int  kbd_ready    = 0;
static uint8_t kbd_prev[8];

// HID keycode tables and decoding now live in usb_hid.c (shared with xHCI).

// ---------------------------------------------------------------------------
// Configure a keyboard device: get descriptor, assign address, set config+protocol
// The device starts at address 0; new_addr is what we assign it.
// ---------------------------------------------------------------------------

static int setup_keyboard(int new_addr, int ep0_mps, int lowspeed) {
    uart_puts("[USB] configuring keyboard...\n");

    // GET_DESCRIPTOR (device) — may need retries after reset
    int r = -1;
    for (int attempt = 0; attempt < 10 && r < 0; attempt++) {
        r = ctrl_xfer(0, ep0_mps, lowspeed, 0x80, 6, (1u<<8), 0, 18, ctrl_buf);
        if (r < 0) usleep(10000);
    }
    if (r < 0) { uart_puts("[USB] kbd GET_DESCRIPTOR failed\n"); return -1; }

    usb_dev_desc_t *dd = (usb_dev_desc_t *)ctrl_buf;
    ep0_mps = dd->bMaxPacketSize0;
    uart_hex("[USB] kbd VID: ", dd->idVendor);
    uart_hex("[USB] kbd PID: ", dd->idProduct);
    uart_dec("[USB] kbd ep0 mps: ", ep0_mps);

    // SET_ADDRESS
    r = ctrl_xfer(0, ep0_mps, lowspeed, 0x00, 5, new_addr, 0, 0, 0);
    if (r < 0) { uart_puts("[USB] kbd SET_ADDRESS failed\n"); return -2; }
    usleep(5000);
    kbd_devaddr = new_addr;

    // GET_DESCRIPTOR (config, 9 bytes first for wTotalLength)
    uart_puts("[USB] kbd config(9)...\n");
    r = ctrl_xfer(new_addr, ep0_mps, lowspeed, 0x80, 6, (2u<<8), 0, 9, ctrl_buf);
    if (r < 0) { uart_puts("[USB] kbd config(9) failed\n"); return -3; }
    uint16_t tlen = ((usb_cfg_desc_t *)ctrl_buf)->wTotalLength;
    uart_dec("[USB] kbd config tlen: ", tlen);
    if (tlen > sizeof(ctrl_buf)) tlen = sizeof(ctrl_buf);

    // GET_DESCRIPTOR (config, full)
    uart_puts("[USB] kbd config(full)...\n");
    r = ctrl_xfer(new_addr, ep0_mps, lowspeed, 0x80, 6, (2u<<8), 0, tlen, ctrl_buf);
    if (r < 0) { uart_puts("[USB] kbd config(full) failed\n"); return -4; }
    uart_puts("[USB] kbd config(full) done\n");

    // Parse: find HID (class=3) interrupt IN endpoint.
    // NOTE: read multi-byte fields byte-by-byte (the descriptor is not aligned,
    // and with the MMU off an unaligned 16/32-bit access would fault).
    int ep_found = 0;
    int in_hid_if = 0;
    uint8_t *p = ctrl_buf, *end = ctrl_buf + tlen;
    while (p < end && !ep_found) {
        if (p + 2 > end) break;
        uint8_t bLen = p[0], bType = p[1];
        if (bLen < 2 || p + bLen > end) break;
        if (bType == 4) {  // interface
            in_hid_if = (p[5] == 3);  // bInterfaceClass == HID
        } else if (bType == 5 && in_hid_if) {  // endpoint in HID interface
            uint32_t ea = p[2];   // bEndpointAddress
            uint32_t at = p[3];   // bmAttributes
            if ((ea & 0x80) && (at & 3) == 3) {  // IN, interrupt
                kbd_ep  = ea & 0x0F;
                kbd_mps = (p[4] | ((uint32_t)p[5] << 8)) & 0x7FF;
                if (kbd_mps == 0) kbd_mps = 8;
                ep_found = 1;
                uart_dec("[USB] kbd EP: ", kbd_ep);
                uart_dec("[USB] kbd EP mps: ", kbd_mps);
            }
        }
        p += bLen;
    }

    if (!ep_found) { uart_puts("[USB] no HID IN endpoint found\n"); return -5; }

    // SET_CONFIGURATION 1
    uart_puts("[USB] kbd SET_CONFIG...\n");
    r = ctrl_xfer(new_addr, ep0_mps, lowspeed, 0x00, 9, 1, 0, 0, 0);
    if (r < 0) { uart_puts("[USB] kbd SET_CONFIG failed\n"); return -6; }
    uart_puts("[USB] kbd SET_CONFIG done\n");
    usleep(5000);

    // SET_PROTOCOL 0 (boot protocol, non-fatal if fails)
    uart_puts("[USB] kbd SET_PROTOCOL...\n");
    ctrl_xfer(new_addr, ep0_mps, lowspeed, 0x21, 0x0B, 0, 0, 0, 0);
    uart_puts("[USB] kbd SET_PROTOCOL done\n");

    kbd_lowspeed = lowspeed;
    kbd_pid      = PID_DATA0;
    kbd_ready    = 1;
    uart_puts("[USB] keyboard configured and ready\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Enumerate a hub and find the keyboard behind it
// hub_devaddr: assign the hub this address
// hub_ep0_mps: initial EP0 MPS for hub (from device descriptor)
// ---------------------------------------------------------------------------

static int enumerate_hub(int hub_devaddr, int hub_ep0_mps) {
    uart_puts("[HUB] enumerating hub...\n");

    // SET_ADDRESS for hub
    int r = ctrl_xfer(0, hub_ep0_mps, 0, 0x00, 5, hub_devaddr, 0, 0, 0);
    if (r < 0) { uart_puts("[HUB] SET_ADDRESS failed\n"); return -1; }
    usleep(5000);

    // GET_DESCRIPTOR (hub descriptor, type=0x29)
    r = ctrl_xfer(hub_devaddr, hub_ep0_mps, 0, 0xA0, 6, (0x29u<<8), 0, 8, hub_buf);
    if (r < 0) { uart_puts("[HUB] GET_DESCRIPTOR hub failed (non-fatal)\n"); }
    int nports = (r == 0) ? hub_buf[2] : 4;
    if (nports == 0 || nports > 8) nports = 4;
    uart_dec("[HUB] number of ports: ", nports);

    // SET_CONFIGURATION 1
    r = ctrl_xfer(hub_devaddr, hub_ep0_mps, 0, 0x00, 9, 1, 0, 0, 0);
    if (r < 0) { uart_puts("[HUB] SET_CONFIG failed\n"); return -2; }
    usleep(5000);

    // Power all ports
    for (int p = 1; p <= nports; p++) {
        hub_set_feat(hub_devaddr, hub_ep0_mps, p, HUB_FEAT_PORT_POWER);
    }
    usleep(200000);  // bPwrOn2PwrGood * 2ms (max 100ms typically)

    // Find and enumerate keyboard on each port
    for (int p = 1; p <= nports; p++) {
        uint32_t ps = 0;
        if (hub_port_status(hub_devaddr, hub_ep0_mps, p, &ps) < 0) continue;
        uart_hex("[HUB] port status: ", ps);

        int connected = ps & PORT_STS_CONNECTION;
        int chg_conn  = ps & PORT_CHG_CONNECTION;

        if (!connected && !chg_conn) continue;

        uart_dec("[HUB] device on port: ", p);

        // Clear the connection change flag if set
        if (chg_conn)
            hub_clr_feat(hub_devaddr, hub_ep0_mps, p, HUB_FEAT_C_CONNECTION);

        // Reset the port to put the device into default state (addr=0)
        uart_puts("[HUB] resetting port...\n");
        hub_set_feat(hub_devaddr, hub_ep0_mps, p, HUB_FEAT_PORT_RESET);

        // Wait for reset-complete change (C_PORT_RESET)
        for (int i = 0; i < 200; i++) {
            usleep(5000);
            if (hub_port_status(hub_devaddr, hub_ep0_mps, p, &ps) < 0) break;
            if (ps & PORT_CHG_RESET) break;
        }
        hub_clr_feat(hub_devaddr, hub_ep0_mps, p, HUB_FEAT_C_RESET);
        usleep(20000);  // recovery time after reset

        // Re-check port status
        if (hub_port_status(hub_devaddr, hub_ep0_mps, p, &ps) == 0) {
            uart_hex("[HUB] port after reset: ", ps);
        }

        // Keyboard device is now at addr=0 on this port; next free address = 2
        int kbd_new_addr = hub_devaddr + 1;
        r = setup_keyboard(kbd_new_addr, 8, 0);
        if (r == 0) return 0;

        uart_puts("[HUB] not a keyboard on this port, continuing\n");
    }

    uart_puts("[HUB] no keyboard found behind hub\n");
    return -3;
}

// ---------------------------------------------------------------------------
// DWC2 host-mode initialisation
// ---------------------------------------------------------------------------

int usb_kbd_init(void) {
    uart_puts("[DWC2] init\n");

    dwc2_power_on();                 // power + ungate clock (real hardware)
    if (dwc2_reset() < 0) return 0;
    uart_puts("[DWC2] reset done\n");

    // Force host mode
    GUSBCFG = (GUSBCFG & ~((1u<<30)|(1u<<29))) | (1u<<29);
    usleep(50000);

    // DMA enable, single burst
    GAHBCFG = (1u << 5);
    GINTMSK = 0;
    GINTSTS = 0xFFFFFFFFu;

    // Configure FIFOs
    GRXFSIZ   = 256;
    GNPTXFSIZ = (256u << 16) | 256u;
    HPTXFSIZ  = (256u << 16) | 512u;

    dwc2_flush_tx();
    dwc2_flush_rx();

    // Power port
    if (!(HPRT & (1u<<12))) {
        hprt_set(1u << 12);
        usleep(20000);
    }

    // Wait for device connect
    uart_puts("[DWC2] waiting for device...\n");
    for (int i = 0; i < 3000; i++) {
        if (HPRT & 1u) break;
        usleep(1000);
    }
    if (!(HPRT & 1u)) { uart_puts("[DWC2] no device\n"); return 0; }
    uart_puts("[DWC2] device connected\n");

    usleep(200000);

    // Port reset
    hprt_set(1u << 8);
    usleep(60000);
    hprt_clr(1u << 8);
    usleep(100000);

    uart_hex("[DWC2] HPRT: ", HPRT);

    // Enumerate: GET_DESCRIPTOR at address 0 to identify the device
    uart_puts("[USB] GET_DESCRIPTOR @ addr 0\n");
    int r = ctrl_xfer(0, 64, 0, 0x80, 6, (1u<<8), 0, 18, ctrl_buf);
    if (r < 0) {
        // Retry with smaller initial MPS
        r = ctrl_xfer(0, 8, 0, 0x80, 6, (1u<<8), 0, 8, ctrl_buf);
    }
    if (r < 0) { uart_puts("[USB] initial GET_DESCRIPTOR failed\n"); return 0; }

    usb_dev_desc_t *dd = (usb_dev_desc_t *)ctrl_buf;
    int ep0_mps = dd->bMaxPacketSize0;
    uart_hex("[USB] VID: ", dd->idVendor);
    uart_hex("[USB] PID: ", dd->idProduct);
    uart_dec("[USB] class: ", dd->bDeviceClass);
    uart_dec("[USB] ep0 mps: ", ep0_mps);

    // Parse the config to check interface class
    int dev_class = dd->bDeviceClass;

    // Some devices report class 0 at device level; get config to check interface
    if (dev_class == 0 || dev_class == 0xFF) {
        // Try GET_DESCRIPTOR config (9 bytes) to peek at interface class
        uint8_t tmp[9] __attribute__((aligned(4)));
        if (ctrl_xfer(0, ep0_mps, 0, 0x80, 6, (2u<<8), 0, 9, tmp) == 0) {
            // wTotalLength is at offset 2; walk just the first interface
            uint16_t tlen = *(uint16_t *)(tmp + 2);
            if (tlen > sizeof(ctrl_buf)) tlen = sizeof(ctrl_buf);
            if (ctrl_xfer(0, ep0_mps, 0, 0x80, 6, (2u<<8), 0, tlen, ctrl_buf) == 0) {
                uint8_t *p = ctrl_buf, *end = ctrl_buf + tlen;
                while (p < end) {
                    if (p[0] < 2 || p + p[0] > end) break;
                    if (p[1] == 4) { dev_class = p[5]; break; }  // interface descriptor
                    p += p[0];
                }
            }
        }
    }

    uart_dec("[USB] effective class: ", dev_class);

    if (dev_class == 9) {
        // USB hub — enumerate it, then find keyboard behind it
        return enumerate_hub(1, ep0_mps) == 0 ? 1 : 0;
    } else if (dev_class == 3) {
        // HID device directly on root port — try as keyboard
        return setup_keyboard(1, ep0_mps, 0) == 0 ? 1 : 0;
    } else {
        uart_puts("[USB] unknown device class, trying as keyboard\n");
        return setup_keyboard(1, ep0_mps, 0) == 0 ? 1 : 0;
    }
}

// ---------------------------------------------------------------------------
// Poll keyboard
// ---------------------------------------------------------------------------

char usb_kbd_getchar(void) {
    if (!kbd_ready) return 0;

    int r = dwc2_xfer(1, kbd_devaddr, kbd_ep, 1 /*IN*/, EP_INTR,
                      kbd_mps, kbd_pid, kbd_report, kbd_mps, kbd_lowspeed);

    if (r == -4) return 0;  // NAK = no key

    if (r < 0) return 0;

    kbd_pid = (kbd_pid == PID_DATA0) ? PID_DATA1 : PID_DATA0;

    return hid_report_char(kbd_report, kbd_prev);
}
