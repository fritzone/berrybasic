#include "xhci.h"
#include "usb_hid.h"
#include "uart.h"
#include "mmu.h"

// ---------------------------------------------------------------------------
// Minimal xHCI driver: enough to enumerate and poll a single USB boot keyboard
// on the VL805 (Pi 4 USB-A ports). Only the happy path is implemented; every
// step is UART-logged because this runs only on real hardware.
//
// All controller-visible structures (rings, contexts, buffers) live in a
// dedicated arena that is marked non-cacheable so the device sees CPU writes
// without explicit cache maintenance.
// ---------------------------------------------------------------------------

// --- register access --------------------------------------------------------
static uintptr_t cap_base;       // capability registers
static uintptr_t op_base;        // operational registers
static uintptr_t rt_base;        // runtime registers
static uintptr_t db_base;        // doorbell array

#define RD32(a)     (*(volatile uint32_t *)(a))
#define WR32(a, v)  (*(volatile uint32_t *)(a) = (v))

// Capability registers
#define CAP_CAPLENGTH   0x00
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

// Operational registers (relative to op_base)
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_PAGESIZE     0x08
#define OP_CRCR         0x18
#define OP_DCBAAP       0x30
#define OP_CONFIG       0x38
#define OP_PORTSC(p)    (0x400 + ((p) - 1) * 0x10)

#define USBCMD_RS       (1u << 0)
#define USBCMD_HCRST    (1u << 1)
#define USBSTS_HCH      (1u << 0)
#define USBSTS_CNR      (1u << 11)

#define PORTSC_CCS      (1u << 0)    // current connect status
#define PORTSC_PED      (1u << 1)    // port enabled
#define PORTSC_PR       (1u << 4)    // port reset
#define PORTSC_PRC      (1u << 21)   // port reset change
#define PORTSC_CSC      (1u << 17)   // connect status change

// Runtime interrupter 0 registers (relative to rt_base)
#define IR0_IMAN        0x20
#define IR0_IMOD        0x24
#define IR0_ERSTSZ      0x28
#define IR0_ERSTBA      0x30
#define IR0_ERDP        0x38

// TRB types
#define TRB_NORMAL          1
#define TRB_SETUP           2
#define TRB_DATA            3
#define TRB_STATUS          4
#define TRB_LINK            6
#define TRB_ENABLE_SLOT     9
#define TRB_ADDRESS_DEVICE  11
#define TRB_CONFIGURE_EP    12
#define TRB_TRANSFER_EVENT  32
#define TRB_CMD_COMPLETION  33
#define TRB_PORT_STATUS     34

#define TRB_TYPE(t)     ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c) (((c) >> 10) & 0x3f)
#define TRB_CYCLE       (1u << 0)
#define TRB_IOC         (1u << 5)    // interrupt on completion
#define TRB_IDT         (1u << 6)    // immediate data
#define COMP_CODE(s)    (((s) >> 24) & 0xff)

typedef struct __attribute__((packed)) {
    uint32_t lo, hi, status, control;
} trb_t;

#define RING_TRBS  16

// --- non-cacheable DMA arena ------------------------------------------------
#define ARENA_SIZE (256 * 1024)
static uint8_t arena[ARENA_SIZE] __attribute__((aligned(0x10000)));
static uint32_t arena_off;

static void *dma_alloc(uint32_t size, uint32_t align) {
    arena_off = (arena_off + align - 1) & ~(align - 1);
    void *p = &arena[arena_off];
    arena_off += size;
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < size; i++) b[i] = 0;
    return p;
}

// --- driver state -----------------------------------------------------------
static uint64_t *dcbaa;
static trb_t    *cmd_ring;
static uint32_t  cmd_enq;
static uint32_t  cmd_cycle;
static trb_t    *event_ring;
static uint32_t  event_deq;
static uint32_t  event_cycle;
static trb_t    *ep0_ring;
static uint32_t  ep0_enq, ep0_cycle;
static trb_t    *epint_ring;
static uint32_t  epint_enq, epint_cycle;
static uint8_t  *dev_ctx;
static uint8_t  *input_ctx;
static int       ctx_size;       // 32 or 64
static int       slot_id;
static int       kbd_dci;        // endpoint DCI of the interrupt IN endpoint
static int       kbd_ready;
static uint8_t   kbd_prev[8];
static uint8_t  *report_buf;     // 8-byte HID report DMA buffer

static void usleep(uint32_t us) {
    volatile uint32_t *clo = (volatile uint32_t *)0xFE003004UL;
    uint32_t t = *clo;
    while (*clo - t < us) { }
}

static uint64_t pa(void *p) { return (uint64_t)(uintptr_t)p; }

// Ring a doorbell (slot 0 = command ring; slot N = device slot, target = DCI).
static void ring_db(int slot, uint32_t target) {
    WR32(db_base + slot * 4, target);
}

// Poll the event ring for the next event TRB; returns 0 on timeout, else 1 and
// fills *out. Advances the dequeue pointer and updates ERDP.
static int next_event(trb_t *out, int timeout_ms) {
    for (int i = 0; i < timeout_ms * 10; i++) {
        trb_t *e = &event_ring[event_deq];
        if ((e->control & TRB_CYCLE) == event_cycle) {
            out->lo = e->lo; out->hi = e->hi;
            out->status = e->status; out->control = e->control;
            if (++event_deq >= RING_TRBS) { event_deq = 0; event_cycle ^= TRB_CYCLE; }
            uint64_t erdp = pa(&event_ring[event_deq]) | (1u << 3);  // EHB clear
            WR32(rt_base + IR0_ERDP, (uint32_t)erdp);
            WR32(rt_base + IR0_ERDP + 4, (uint32_t)(erdp >> 32));
            return 1;
        }
        usleep(100);
    }
    return 0;
}

// Enqueue a TRB on a transfer/command ring (with link-TRB wrap at the end).
static void ring_push(trb_t *ring, uint32_t *enq, uint32_t *cyc,
                      uint64_t param, uint32_t status, uint32_t control) {
    trb_t *t = &ring[*enq];
    t->lo = (uint32_t)param;
    t->hi = (uint32_t)(param >> 32);
    t->status = status;
    t->control = control | *cyc;
    if (++(*enq) >= RING_TRBS - 1) {
        // Link TRB back to start, toggling the cycle.
        trb_t *l = &ring[RING_TRBS - 1];
        l->lo = (uint32_t)pa(ring);
        l->hi = (uint32_t)(pa(ring) >> 32);
        l->status = 0;
        l->control = TRB_TYPE(TRB_LINK) | (1u << 1) /*TC*/ | *cyc;
        *enq = 0;
        *cyc ^= TRB_CYCLE;
    }
}

// Issue a command TRB and wait for its completion event. Returns the slot id
// from the completion (or completion code in low bits); <0 on error.
static int run_command(uint64_t param, uint32_t control) {
    ring_push(cmd_ring, &cmd_enq, &cmd_cycle, param, 0, control);
    ring_db(0, 0);
    trb_t ev;
    if (!next_event(&ev, 200)) { uart_puts("[XHCI] cmd timeout\n"); return -1; }
    if (TRB_GET_TYPE(ev.control) != TRB_CMD_COMPLETION) {
        uart_hex("[XHCI] unexpected event ", TRB_GET_TYPE(ev.control));
        return -1;
    }
    int cc = COMP_CODE(ev.status);
    if (cc != 1) { uart_hex("[XHCI] cmd comp code ", cc); return -1; }
    return (ev.control >> 24) & 0xff;     // slot id
}

// --- control transfer on EP0 ------------------------------------------------
// Returns 0 on success, <0 on error. `buf` (in the arena) holds in/out data.
static int control_xfer(uint8_t bmReqType, uint8_t bReq, uint16_t wValue,
                        uint16_t wIndex, uint16_t wLength, void *buf) {
    uint64_t setup = (uint64_t)bmReqType | ((uint64_t)bReq << 8) |
                     ((uint64_t)wValue << 16) | ((uint64_t)wIndex << 32) |
                     ((uint64_t)wLength << 48);
    int in = (bmReqType & 0x80) != 0;

    // Setup stage (immediate data).
    uint32_t trt = wLength ? (in ? 3 : 2) : 0;       // transfer type
    ring_push(ep0_ring, &ep0_enq, &ep0_cycle, setup, 8,
              TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
    // Data stage.
    if (wLength)
        ring_push(ep0_ring, &ep0_enq, &ep0_cycle, pa(buf), wLength,
                  TRB_TYPE(TRB_DATA) | (in ? (1u << 16) : 0));
    // Status stage (opposite direction, IOC).
    ring_push(ep0_ring, &ep0_enq, &ep0_cycle, 0, 0,
              TRB_TYPE(TRB_STATUS) | ((wLength && in) ? 0 : (1u << 16)) | TRB_IOC);

    ring_db(slot_id, 1);                              // EP0 = DCI 1
    trb_t ev;
    if (!next_event(&ev, 200)) { uart_puts("[XHCI] ctrl timeout\n"); return -1; }
    int cc = COMP_CODE(ev.status);
    if (cc != 1 && cc != 13 /*short packet*/) { uart_hex("[XHCI] ctrl cc ", cc); return -1; }
    return 0;
}

// Slot/endpoint context field helpers (32 dwords each, ctx_size bytes apart).
static uint32_t *slot_ctx(uint8_t *ctx)        { return (uint32_t *)ctx; }
static uint32_t *ep_ctx(uint8_t *ctx, int dci) { return (uint32_t *)(ctx + dci * ctx_size); }

// --- controller bring-up ----------------------------------------------------
static int xhci_start(void) {
    uint8_t caplen = RD32(cap_base + CAP_CAPLENGTH) & 0xff;
    op_base = cap_base + caplen;
    rt_base = cap_base + (RD32(cap_base + CAP_RTSOFF) & ~0x1fu);
    db_base = cap_base + (RD32(cap_base + CAP_DBOFF) & ~0x3u);

    uint32_t hcc = RD32(cap_base + CAP_HCCPARAMS1);
    ctx_size = (hcc & (1u << 2)) ? 64 : 32;
    uint32_t hcs1 = RD32(cap_base + CAP_HCSPARAMS1);
    int maxslots = hcs1 & 0xff;
    int maxports = (hcs1 >> 24) & 0xff;
    uart_dec("[XHCI] max slots: ", maxslots);
    uart_dec("[XHCI] max ports: ", maxports);

    // Halt then reset the controller.
    uint32_t cmd = RD32(op_base + OP_USBCMD);
    WR32(op_base + OP_USBCMD, cmd & ~USBCMD_RS);
    for (int i = 0; i < 100 && !(RD32(op_base + OP_USBSTS) & USBSTS_HCH); i++) usleep(1000);
    WR32(op_base + OP_USBCMD, USBCMD_HCRST);
    for (int i = 0; i < 1000 && (RD32(op_base + OP_USBCMD) & USBCMD_HCRST); i++) usleep(1000);
    for (int i = 0; i < 1000 && (RD32(op_base + OP_USBSTS) & USBSTS_CNR); i++) usleep(1000);
    if (RD32(op_base + OP_USBSTS) & USBSTS_CNR) { uart_puts("[XHCI] controller not ready\n"); return -1; }

    // DCBAA, command ring, event ring, scratchpad.
    dcbaa = dma_alloc((maxslots + 1) * 8, 64);
    cmd_ring = dma_alloc(RING_TRBS * sizeof(trb_t), 64);
    event_ring = dma_alloc(RING_TRBS * sizeof(trb_t), 64);
    cmd_enq = 0; cmd_cycle = TRB_CYCLE;
    event_deq = 0; event_cycle = TRB_CYCLE;

    // Scratchpad buffers if the controller wants them.
    uint32_t hcs2 = RD32(cap_base + CAP_HCSPARAMS2);
    int spb = ((hcs2 >> 27) & 0x1f) | (((hcs2 >> 21) & 0x1f) << 5);
    if (spb) {
        uint64_t *spa = dma_alloc(spb * 8, 64);
        for (int i = 0; i < spb; i++) spa[i] = pa(dma_alloc(4096, 4096));
        dcbaa[0] = pa(spa);
    }
    uart_dec("[XHCI] scratchpad bufs: ", spb);

    WR32(op_base + OP_DCBAAP, (uint32_t)pa(dcbaa));
    WR32(op_base + OP_DCBAAP + 4, (uint32_t)(pa(dcbaa) >> 32));
    WR32(op_base + OP_CONFIG, maxslots);

    uint64_t crcr = pa(cmd_ring) | TRB_CYCLE;   // RCS=1
    WR32(op_base + OP_CRCR, (uint32_t)crcr);
    WR32(op_base + OP_CRCR + 4, (uint32_t)(crcr >> 32));

    // Event ring segment table (one segment).
    uint32_t *erst = dma_alloc(16, 64);
    erst[0] = (uint32_t)pa(event_ring);
    erst[1] = (uint32_t)(pa(event_ring) >> 32);
    erst[2] = RING_TRBS;     // segment size
    erst[3] = 0;
    WR32(rt_base + IR0_ERSTSZ, 1);
    uint64_t erdp = pa(event_ring);
    WR32(rt_base + IR0_ERDP, (uint32_t)erdp);
    WR32(rt_base + IR0_ERDP + 4, (uint32_t)(erdp >> 32));
    uint64_t erstba = pa(erst);
    WR32(rt_base + IR0_ERSTBA, (uint32_t)erstba);
    WR32(rt_base + IR0_ERSTBA + 4, (uint32_t)(erstba >> 32));

    // Run.
    WR32(op_base + OP_USBCMD, USBCMD_RS);
    for (int i = 0; i < 100 && (RD32(op_base + OP_USBSTS) & USBSTS_HCH); i++) usleep(1000);
    uart_puts("[XHCI] controller running\n");
    return maxports;
}

// Reset a port and return 1 if a device is present and enabled.
static int port_reset(int port) {
    uint32_t sc = RD32(op_base + OP_PORTSC(port));
    if (!(sc & PORTSC_CCS)) return 0;               // nothing connected
    // Write 1 to PR; preserve, clear change bits by writing them back.
    WR32(op_base + OP_PORTSC(port), (sc & ~PORTSC_PED) | PORTSC_PR | PORTSC_CSC);
    for (int i = 0; i < 200; i++) {
        sc = RD32(op_base + OP_PORTSC(port));
        if (sc & PORTSC_PRC) break;
        usleep(1000);
    }
    WR32(op_base + OP_PORTSC(port), sc | PORTSC_PRC); // ack reset change
    usleep(20000);
    sc = RD32(op_base + OP_PORTSC(port));
    return (sc & PORTSC_PED) ? 1 : 0;
}

int xhci_kbd_init(uintptr_t mmio_base) {
    cap_base = mmio_base;
    arena_off = 0;
    kbd_ready = 0;

    // The arena must be non-cacheable so the controller sees our writes.
    mmu_set_noncached(pa(arena), ARENA_SIZE);

    int maxports = xhci_start();
    if (maxports < 0) return 0;

    // Find and reset a connected port.
    int port = 0;
    for (int p = 1; p <= maxports; p++) {
        if (RD32(op_base + OP_PORTSC(p)) & PORTSC_CCS) { port = p; break; }
    }
    if (!port) { uart_puts("[XHCI] no device on any port\n"); return 0; }
    uart_dec("[XHCI] device on port ", port);
    if (!port_reset(port)) { uart_puts("[XHCI] port reset failed\n"); return 0; }

    // Enable a slot.
    slot_id = run_command(0, TRB_TYPE(TRB_ENABLE_SLOT));
    if (slot_id <= 0) { uart_puts("[XHCI] enable slot failed\n"); return 0; }
    uart_dec("[XHCI] slot ", slot_id);

    // Device + input contexts, EP0 transfer ring.
    dev_ctx   = dma_alloc(32 * ctx_size, 64);
    input_ctx = dma_alloc(33 * ctx_size, 64);
    ep0_ring  = dma_alloc(RING_TRBS * sizeof(trb_t), 64);
    ep0_enq = 0; ep0_cycle = TRB_CYCLE;
    dcbaa[slot_id] = pa(dev_ctx);

    // Input control context: add slot (A0) and EP0 (A1).
    uint32_t *icc = (uint32_t *)input_ctx;
    icc[1] = (1u << 0) | (1u << 1);
    // Slot context: 1 entry context, root hub port.
    uint32_t *slot = slot_ctx(input_ctx + ctx_size);
    slot[0] = (1u << 27);                 // context entries = 1
    slot[1] = (port << 16);               // root hub port number
    // EP0 context: control, EP type 4, MPS 8 (full speed default), TR ptr.
    uint32_t *ep0 = ep_ctx(input_ctx + ctx_size, 1);
    ep0[1] = (4u << 3) | (8u << 16) | (3u << 1);   // EP type=control, MPS=8, CErr=3
    uint64_t trp = pa(ep0_ring) | 1;       // DCS=1
    ep0[2] = (uint32_t)trp;
    ep0[3] = (uint32_t)(trp >> 32);

    if (run_command(pa(input_ctx), TRB_TYPE(TRB_ADDRESS_DEVICE) | (slot_id << 24)) < 0) {
        uart_puts("[XHCI] address device failed\n"); return 0;
    }
    uart_puts("[XHCI] device addressed\n");

    // Get device descriptor (18 bytes) to read EP0 max packet size.
    uint8_t *buf = dma_alloc(256, 64);
    if (control_xfer(0x80, 6, (1 << 8), 0, 18, buf) < 0) {
        uart_puts("[XHCI] GET_DESCRIPTOR failed\n"); return 0;
    }
    int ep0_mps = buf[7];
    uart_hex("[XHCI] VID ", buf[8] | (buf[9] << 8));
    uart_hex("[XHCI] PID ", buf[10] | (buf[11] << 8));
    (void)ep0_mps;   // (could re-evaluate EP0 context; MPS 8 works for enumeration)

    // Get configuration descriptor (first 9 bytes, then full) and find the HID
    // interrupt IN endpoint.
    if (control_xfer(0x80, 6, (2 << 8), 0, 9, buf) < 0) return 0;
    int tlen = buf[2] | (buf[3] << 8);
    if (tlen > 256) tlen = 256;
    if (control_xfer(0x80, 6, (2 << 8), 0, tlen, buf) < 0) return 0;

    int ep_addr = 0, ep_mps = 8, ep_interval = 8, in_hid = 0;
    for (int p = 0; p + 2 <= tlen; ) {
        int blen = buf[p], btype = buf[p + 1];
        if (blen < 2) break;
        if (btype == 4) in_hid = (buf[p + 5] == 3);            // interface, class HID
        else if (btype == 5 && in_hid) {                       // endpoint
            if ((buf[p + 2] & 0x80) && (buf[p + 3] & 3) == 3) {
                ep_addr = buf[p + 2] & 0x0f;
                ep_mps  = (buf[p + 4] | (buf[p + 5] << 8)) & 0x7ff;
                ep_interval = buf[p + 6];
                break;
            }
        }
        p += blen;
    }
    if (!ep_addr) { uart_puts("[XHCI] no HID IN endpoint\n"); return 0; }
    uart_dec("[XHCI] kbd EP ", ep_addr);

    // SET_CONFIGURATION 1, SET_PROTOCOL boot(0).
    if (control_xfer(0x00, 9, 1, 0, 0, buf) < 0) { uart_puts("[XHCI] SET_CONFIG failed\n"); return 0; }
    control_xfer(0x21, 0x0b, 0, 0, 0, buf);                    // SET_PROTOCOL (boot)

    // Configure the interrupt IN endpoint.
    kbd_dci = ep_addr * 2 + 1;                                 // IN endpoint DCI
    epint_ring = dma_alloc(RING_TRBS * sizeof(trb_t), 64);
    epint_enq = 0; epint_cycle = TRB_CYCLE;
    report_buf = dma_alloc(8, 64);

    for (int i = 0; i < 33 * ctx_size; i++) input_ctx[i] = 0;
    icc = (uint32_t *)input_ctx;
    icc[1] = (1u << 0) | (1u << kbd_dci);                      // add slot + this EP
    slot = slot_ctx(input_ctx + ctx_size);
    slot[0] = (kbd_dci << 27);                                 // context entries
    slot[1] = (port << 16);
    uint32_t *epi = ep_ctx(input_ctx + ctx_size, kbd_dci);
    int interval = 6;                                          // ~8ms for FS HID
    if (ep_interval > 0) interval = 3;                         // leave conservative
    epi[0] = (interval << 16);
    epi[1] = (7u << 3) | (ep_mps << 16) | (3u << 1);          // EP type 7 = interrupt IN
    uint64_t itrp = pa(epint_ring) | 1;
    epi[2] = (uint32_t)itrp;
    epi[3] = (uint32_t)(itrp >> 32);
    epi[4] = ep_mps;                                          // average TRB length

    if (run_command(pa(input_ctx), TRB_TYPE(TRB_CONFIGURE_EP) | (slot_id << 24)) < 0) {
        uart_puts("[XHCI] configure EP failed\n"); return 0;
    }

    // Prime the interrupt endpoint with one transfer.
    ring_push(epint_ring, &epint_enq, &epint_cycle, pa(report_buf), 8,
              TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    ring_db(slot_id, kbd_dci);

    kbd_ready = 1;
    uart_puts("[XHCI] keyboard ready\n");
    return 1;
}

char xhci_kbd_getchar(void) {
    if (!kbd_ready) return 0;

    // Poll the event ring briefly; on a completed interrupt transfer, decode the
    // HID report and re-arm the endpoint for the next one.
    trb_t ev;
    char out = 0;
    if (next_event(&ev, 1)) {
        if (TRB_GET_TYPE(ev.control) == TRB_TRANSFER_EVENT) {
            int cc = COMP_CODE(ev.status);
            if (cc == 1 || cc == 13) out = hid_report_char(report_buf, kbd_prev);
            // Re-arm the interrupt endpoint for the next report.
            ring_push(epint_ring, &epint_enq, &epint_cycle, pa(report_buf), 8,
                      TRB_TYPE(TRB_NORMAL) | TRB_IOC);
            ring_db(slot_id, kbd_dci);
        }
    }
    return out;
}
