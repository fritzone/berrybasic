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
#define USBCMD_INTE     (1u << 2)    // interrupter enable
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
#define TRB_EVALUATE_CONTEXT 13
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
// CRITICAL: mmu_set_noncached() marks whole 2 MiB MMU blocks non-cacheable, so
// the arena MUST occupy its OWN 2 MiB block. If it shared a block with the
// stack / kernel / globals (as a normal BSS array in the low 2 MiB would),
// marking it non-cacheable would make those stale too - corrupting the stack
// (PC alignment fault) and globals like cap_base (garbage MMIO reads). So the
// arena is exactly 2 MiB and 2 MiB-aligned: it fills one block and nothing else
// shares it.
#define ARENA_SIZE (2 * 1024 * 1024)
static uint8_t arena[ARENA_SIZE] __attribute__((aligned(ARENA_SIZE)));
static uint32_t arena_off;

static void *dma_alloc(uint32_t size, uint32_t align) {
    arena_off = (arena_off + align - 1) & ~(align - 1);
    if (arena_off + size > ARENA_SIZE) {       // never write past the arena block
        uart_puts("[XHCI] arena exhausted\n");
        return &arena[0];                       // degrade safely rather than smash memory
    }
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
static trb_t    *epint_ring;     // scratch during endpoint setup, then snapshotted
static uint32_t  epint_enq, epint_cycle;
static uint8_t  *dev_ctx;
static uint8_t  *input_ctx;
static int       ctx_size;       // 32 or 64
static int       slot_id;
static uint8_t  *report_buf;     // HID report DMA buffer (scratch during setup)

// Per-device interrupt-endpoint polling state. Snapshotted from the shared setup
// scratch (slot_id / epint_ring / report_buf) at the end of each device's
// enumeration so that configuring a second device (e.g. the mouse) cannot clobber
// the first device's (the keyboard's) polling registers. The keyboard and mouse
// interrupt endpoints post to the SAME event ring, so xhci_pump() demultiplexes
// completions by (slot, endpoint DCI).
typedef struct {
    int      ready;
    int      slot;
    int      dci;
    trb_t   *ring;
    uint32_t enq, cyc;
    uint8_t *buf;
    int      len;        // interrupt transfer length (report size)
} hid_ep_t;

static hid_ep_t kbd_ep_st;
static hid_ep_t mou_ep_st;
static uint8_t  kbd_prev[8];

// The keyboard's *control* endpoint, snapshotted for the same reason hid_ep_t
// exists: address_device_on() hands each new device a fresh ep0_ring and leaves
// it in the globals, so once a second device (the mouse) is enumerated the
// globals no longer describe the keyboard. Setting the lock LEDs is a control
// transfer to the keyboard, long after that, so its EP0 has to be kept.
typedef struct {
    int      ready;
    int      slot;
    int      iface;      // bInterfaceNumber: SET_REPORT is addressed per-interface
    trb_t   *ring;
    uint32_t enq, cyc;
    uint8_t *led;        // 1-byte DMA buffer for the output report
} ctrl_ep_t;

static ctrl_ep_t kbd_ctrl;

// Pending keyboard keys decoded by xhci_pump() (drained by getchar). Small FIFO
// so a key decoded while servicing the ring for the mouse is not lost. Keys are
// int, not char: F-keys and friends live above 0xFF (see usb_hid.h).
#define KEY_FIFO_N 16
static int      key_fifo[KEY_FIFO_N];
static int      key_head, key_tail;

// Accumulated mouse movement decoded by xhci_pump() (drained by xhci_mouse_poll).
static int      mou_have, mou_btn, mou_dx, mou_dy, mou_wheel;

static void usleep(uint32_t us) {
    volatile uint32_t *clo = (volatile uint32_t *)0xFE003004UL;
    uint32_t t = *clo;
    while (*clo - t < us) { }
}

static uint64_t pa(void *p) { return (uint64_t)(uintptr_t)p; }

// Full system barrier: ensure prior memory writes (TRBs, contexts in the
// non-cached arena) are globally visible before the following access.
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

// Ring a doorbell (slot 0 = command ring; slot N = device slot, target = DCI).
// The DSB is essential: the TRB we just wrote lives in non-cached memory and the
// doorbell is a Device write; without the barrier the doorbell can reach the
// controller before the TRB lands, so it reads a stale/zero TRB (e.g. a null
// input-context pointer) and the command never completes.
static void ring_db(int slot, uint32_t target) {
    dsb();
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
// Route a stray transfer-event completion to the keyboard/mouse interrupt EP so a
// HID report that lands while we wait for a command/control completion is decoded
// and its endpoint re-armed (defined far below; forward-declared for the waits).
static void handle_hid_event(int sid, int eid, int cc);

static int run_command(uint64_t param, uint32_t control) {
    // Remember the address of the command TRB we are about to enqueue. The
    // Command Completion Event echoes this pointer, so we can match the
    // completion to OUR command and ignore completions that belong to an
    // earlier, slow-to-finish command (otherwise the event pipeline shifts by
    // one and every later wait reads the wrong event).
    uint64_t my_trb = pa(&cmd_ring[cmd_enq]);
    ring_push(cmd_ring, &cmd_enq, &cmd_cycle, param, 0, control);
    ring_db(0, 0);
    volatile uint32_t *clo = (volatile uint32_t *)0xFE003004UL;
    uint32_t t0 = *clo;
    trb_t ev;
    // Poll in short slices, re-ringing the command doorbell between them. After
    // the command ring goes idle (following the previous command), a single
    // doorbell can be missed by the controller; periodically re-ringing nudges a
    // stalled command ring without re-executing already-finished commands.
    for (int tries = 0; tries < 60; tries++) {     // ~3 s total
        if (!next_event(&ev, 50)) {
            ring_db(0, 0);                          // nudge a possibly-stalled ring
            continue;
        }
        if (TRB_GET_TYPE(ev.control) == TRB_CMD_COMPLETION) {
            uint64_t ev_trb = (uint64_t)ev.lo | ((uint64_t)ev.hi << 32);
            if (ev_trb != my_trb) {            // completion for a previous command
                uart_hex("[XHCI] stale cmd comp ", (uint32_t)ev_trb);
                continue;
            }
            uart_dec("[XHCI] cmd us ", *clo - t0);
            int cc = COMP_CODE(ev.status);
            if (cc != 1) { uart_hex("[XHCI] cmd comp code ", cc); return -1; }
            return (ev.control >> 24) & 0xff;     // slot id
        }
        if (TRB_GET_TYPE(ev.control) == TRB_TRANSFER_EVENT)   // HID report arrived mid-command
            handle_hid_event((ev.control >> 24) & 0xff, (ev.control >> 16) & 0x1f, COMP_CODE(ev.status));
    }
    uart_puts("[XHCI] cmd timeout\n");
    uart_hex("[XHCI]   USBSTS: ", RD32(op_base + OP_USBSTS));
    uart_hex("[XHCI]   CRCR lo: ", RD32(op_base + OP_CRCR));
    uart_hex("[XHCI]   PORTSC1: ", RD32(op_base + OP_PORTSC(1)));
    return -1;
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
    // Drain until the Transfer Event, re-ringing the endpoint doorbell between
    // polls (same lost-doorbell race as the command ring) and skipping any
    // intervening events (e.g. Port Status Change).
    for (int tries = 0; tries < 60; tries++) {        // ~3 s total
        if (!next_event(&ev, 50)) {
            ring_db(slot_id, 1);                       // nudge a possibly-stalled ring
            continue;
        }
        if (TRB_GET_TYPE(ev.control) == TRB_TRANSFER_EVENT) {
            int sid = (ev.control >> 24) & 0xff;
            int eid = (ev.control >> 16) & 0x1f;
            int cc  = COMP_CODE(ev.status);
            if (sid == slot_id && eid == 1) {            // OUR EP0 completion
                if (cc != 1 && cc != 13 /*short packet*/) { uart_hex("[XHCI] ctrl cc ", cc); return -1; }
                return 0;
            }
            handle_hid_event(sid, eid, cc);              // a HID report mid-transfer - keep it alive
        }
    }
    uart_puts("[XHCI] ctrl timeout\n");
    return -1;
}

// Slot/endpoint context field helpers (32 dwords each, ctx_size bytes apart).
static uint32_t *slot_ctx(uint8_t *ctx)        { return (uint32_t *)ctx; }
static uint32_t *ep_ctx(uint8_t *ctx, int dci) { return (uint32_t *)(ctx + dci * ctx_size); }

// --- controller bring-up ----------------------------------------------------
static int xhci_start(void) {
    uint32_t cap0 = RD32(cap_base + CAP_CAPLENGTH);
    uint8_t  caplen  = cap0 & 0xff;
    uint16_t hciver  = (cap0 >> 16) & 0xffff;
    uart_hex("[XHCI] cap0 raw: ", cap0);
    uart_hex("[XHCI] HCIVERSION: ", hciver);
    // Sanity: a working xHCI has CAPLENGTH ~0x20-0x40 and HCIVERSION 0x0100/0x0110.
    // If these are wild, the MMIO window isn't reaching the controller - bail
    // gracefully rather than computing a misaligned op_base and data-aborting.
    if (caplen < 0x20 || caplen > 0x40 || (hciver != 0x0100 && hciver != 0x0110)) {
        uart_puts("[XHCI] MMIO not responding (bad CAPLENGTH/HCIVERSION)\n");
        return -1;
    }
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

    uart_hex("[XHCI] USBSTS pre : ", RD32(op_base + OP_USBSTS));

    // CRITICAL: the VL805 sets CNR (Controller Not Ready) while its firmware
    // (re)loads after the reset-notify. The xHCI spec forbids writing ANY
    // register except USBSTS while CNR=1; writing USBCMD too early wedges the
    // controller (HSE set, HCRST never clears). So wait for CNR to clear FIRST.
    int i;
    for (i = 0; i < 8000 && (RD32(op_base + OP_USBSTS) & USBSTS_CNR); i++) usleep(1000);
    uart_dec("[XHCI] initial CNR clear after ms ", i);
    if (RD32(op_base + OP_USBSTS) & USBSTS_CNR) {
        uart_puts("[XHCI] controller never became ready (VL805 firmware?)\n");
        return -1;
    }

    // Now it is safe to halt: clear Run/Stop, wait for HCHalted.
    uint32_t cmd = RD32(op_base + OP_USBCMD);
    WR32(op_base + OP_USBCMD, cmd & ~USBCMD_RS);
    for (i = 0; i < 200 && !(RD32(op_base + OP_USBSTS) & USBSTS_HCH); i++) usleep(1000);
    uart_hex("[XHCI] USBSTS halt: ", RD32(op_base + OP_USBSTS));

    // Reset: set HCRST and wait for it to self-clear, then for CNR to clear again.
    WR32(op_base + OP_USBCMD, USBCMD_HCRST);
    for (i = 0; i < 1000 && (RD32(op_base + OP_USBCMD) & USBCMD_HCRST); i++) usleep(1000);
    uart_dec("[XHCI] HCRST cleared after ms ", i);
    for (i = 0; i < 2000 && (RD32(op_base + OP_USBSTS) & USBSTS_CNR); i++) usleep(1000);
    uart_dec("[XHCI] CNR cleared after ms ", i);
    uart_hex("[XHCI] USBSTS post: ", RD32(op_base + OP_USBSTS));
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
    if (spb > 64) spb = 64;                 // sanity clamp (real controllers use few)
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

    // Interrupter 0: no moderation (IMOD=0 -> post events immediately) and
    // enable it (IMAN.IE=1, write 1 to IP to clear). We poll the event ring, but
    // the VL805 appears to defer posting completion events for ~1 s unless the
    // interrupter is enabled and moderation is disabled.
    WR32(rt_base + IR0_IMOD, 0);
    WR32(rt_base + IR0_IMAN, 0x3);

    // Run (Run/Stop + Interrupter Enable).
    WR32(op_base + OP_USBCMD, USBCMD_RS | USBCMD_INTE);
    for (int i = 0; i < 100 && (RD32(op_base + OP_USBSTS) & USBSTS_HCH); i++) usleep(1000);
    uart_puts("[XHCI] controller running\n");
    return maxports;
}

// PORTSC has dangerous bits when writing: PED (bit 1) is write-1-to-DISABLE, and
// the change bits [23:17] are write-1-to-clear. So when we write to set PR we
// must mask both off, or we accidentally disable the port / clear changes.
#define PORTSC_PP        (1u << 9)        // port power
#define PORTSC_CHANGES   (0x7fu << 17)    // CSC,PEC,WRC,OCC,PRC,PLC,CEC
#define PORTSC_PRESERVE  (~(PORTSC_PED | PORTSC_CHANGES))

// Reset a port and return 1 if a device is present and enabled.
static int port_reset(int port) {
    uint32_t sc = RD32(op_base + OP_PORTSC(port));
    uart_hex("[XHCI] PORTSC init  : ", sc);
    if (!(sc & PORTSC_CCS)) return 0;               // nothing connected

    // Ensure the port is powered.
    if (!(sc & PORTSC_PP)) {
        WR32(op_base + OP_PORTSC(port), (sc & PORTSC_PRESERVE) | PORTSC_PP);
        usleep(20000);
        sc = RD32(op_base + OP_PORTSC(port));
    }

    // A SuperSpeed (USB3) port enables itself in hardware on connect - it is
    // already in the Enabled state, no PR needed (and PR would be wrong).
    if (sc & PORTSC_PED) {
        uart_puts("[XHCI] port already enabled (SuperSpeed)\n");
        return 1;
    }

    // USB2 port: issue a port reset (PR) and wait for the reset-change (PRC).
    WR32(op_base + OP_PORTSC(port), (sc & PORTSC_PRESERVE) | PORTSC_PR);
    int i;
    for (i = 0; i < 500; i++) {
        sc = RD32(op_base + OP_PORTSC(port));
        if (sc & PORTSC_PRC) break;
        usleep(1000);
    }
    uart_dec("[XHCI] PRC after ms : ", i);
    uart_hex("[XHCI] PORTSC reset : ", sc);

    // Acknowledge the reset + connect changes (write 1 to clear them only).
    WR32(op_base + OP_PORTSC(port),
         (sc & PORTSC_PRESERVE) | PORTSC_PRC | PORTSC_CSC);
    usleep(20000);
    sc = RD32(op_base + OP_PORTSC(port));
    uart_hex("[XHCI] PORTSC final : ", sc);
    return (sc & PORTSC_PED) ? 1 : 0;
}

// Enable a slot and Address a device, leaving it in the global slot_id with a
// fresh global ep0_ring / dev_ctx / input_ctx.
//   root_port : VL805 root-hub port (1-based) the device's topology hangs off
//   route     : route string (0 = directly on a root port; else parent hub's
//               downstream port number, for a tier-1 device)
//   speed     : 1=FS 2=LS 3=HS 4=SS
//   tt_slot   : parent hub slot id (FS/LS device behind a HS hub) or 0 = no TT
//   tt_port   : parent hub downstream port number (when tt_slot != 0)
// Returns the new slot id, or 0 on failure.
static int address_device_on(int root_port, uint32_t route, int speed,
                             int tt_slot, int tt_port) {
    int sid = run_command(0, TRB_TYPE(TRB_ENABLE_SLOT));
    if (sid <= 0) { uart_puts("[XHCI] enable slot failed\n"); return 0; }
    uart_dec("[XHCI] slot ", sid);

    dev_ctx   = dma_alloc(32 * ctx_size, 64);
    input_ctx = dma_alloc(33 * ctx_size, 64);
    ep0_ring  = dma_alloc(RING_TRBS * sizeof(trb_t), 64);
    ep0_enq = 0; ep0_cycle = TRB_CYCLE;
    dcbaa[sid] = pa(dev_ctx);

    int mps0 = (speed == 4) ? 512 : (speed == 3) ? 64 : 8;

    uint32_t *icc = (uint32_t *)input_ctx;
    icc[1] = (1u << 0) | (1u << 1);                       // add slot + EP0
    uint32_t *slot = slot_ctx(input_ctx + ctx_size);
    slot[0] = (1u << 27) | ((uint32_t)speed << 20) | (route & 0xfffff);
    slot[1] = (root_port << 16);
    if (tt_slot)                                          // FS/LS behind a HS hub
        slot[2] = (tt_slot & 0xff) | ((tt_port & 0xff) << 8);
    uint32_t *ep0 = ep_ctx(input_ctx + ctx_size, 1);
    ep0[1] = (4u << 3) | ((uint32_t)mps0 << 16) | (3u << 1);
    ep0[4] = 8;                                           // Average TRB Length
    uint64_t trp = pa(ep0_ring) | 1;                      // DCS=1
    ep0[2] = (uint32_t)trp;
    ep0[3] = (uint32_t)(trp >> 32);

    if (run_command(pa(input_ctx),
                    TRB_TYPE(TRB_ADDRESS_DEVICE) | (1u << 9) | (sid << 24)) < 0) {
        uart_puts("[XHCI] addr dev BSR=1 failed\n"); return 0;
    }
    if (run_command(pa(input_ctx), TRB_TYPE(TRB_ADDRESS_DEVICE) | (sid << 24)) < 0) {
        uart_puts("[XHCI] address device failed\n"); return 0;
    }
    slot_id = sid;
    uart_puts("[XHCI] device addressed\n");
    return sid;
}

// Update the currently-addressed device's EP0 Max Packet Size via an Evaluate
// Context command. Needed after learning a full-speed device's real
// bMaxPacketSize0 (it may be 8/16/32/64, but we address FS/LS at a provisional
// 8). Reuses the device's input_ctx (still valid from address_device_on).
static int evaluate_ep0_mps(int mps) {
    uint32_t *icc = (uint32_t *)input_ctx;
    icc[0] = 0;
    icc[1] = (1u << 1);                          // evaluate EP0 (DCI 1) only
    uint32_t *ep0 = ep_ctx(input_ctx + ctx_size, 1);
    ep0[1] = (ep0[1] & 0x0000ffffu) | ((uint32_t)mps << 16);   // MPS = bits 31:16
    return run_command(pa(input_ctx),
                       TRB_TYPE(TRB_EVALUATE_CONTEXT) | (slot_id << 24));
}

// Read the 18-byte device descriptor safely. A full-speed device may use an EP0
// Max Packet Size larger than the provisional 8 we addressed it with; issuing an
// 18-byte read against MPS 8 makes such a device babble (it answers in one big
// packet). So fetch the first 8 bytes (always exactly one short reply), take the
// real bMaxPacketSize0 from byte 7, correct EP0 for FS devices, then read the
// full descriptor. `speed`: 1=FS 2=LS 3=HS 4=SS. Returns <0 on error.
static int get_device_descriptor(uint8_t *buf, int speed) {
    // Only full speed is ambiguous: LS is spec-fixed at 8, HS at 64, SS uses the
    // 2^n encoding we already set — for those the provisional MPS is already right,
    // so read straight through exactly as before (no extra traffic/timing change).
    if (speed != 1)
        return control_xfer(0x80, 6, (1 << 8), 0, 18, buf);
    // Full speed: EP0 MPS may be 8/16/32/64 but we addressed at 8. Read 8 bytes
    // first (always exactly one short reply), correct EP0, then read the full 18.
    int r = control_xfer(0x80, 6, (1 << 8), 0, 8, buf);
    if (r < 0) return r;
    if (buf[7] >= 8 && buf[7] != 8) {
        uart_dec("[XHCI] EP0 mps fixup ", buf[7]);
        evaluate_ep0_mps(buf[7]);
    }
    return control_xfer(0x80, 6, (1 << 8), 0, 18, buf);
}

// Configure an already-addressed device as a USB hub and find a device on one of
// its downstream ports. `hub_slot`/`hub_speed`/`route`/`tt_slot`/`tt_port` are the
// hub's own slot context (route and TT must be preserved when we re-write the
// slot context to set the hub bit, or a 2nd-tier hub loses its route). Returns the
// connected downstream port with *dspeed set; 0 if nothing is connected.
// `skip_port` (0 = none) is a downstream port to ignore while scanning - used on
// a second pass to find the OTHER device (e.g. the mouse) on a hub whose first
// device (the keyboard) is already claimed. `first_time` does the one-shot hub
// configuration (SET_CONFIG, mark-as-hub, power ports); on a rescan it is skipped
// (the hub is already up) and the connection scan is brief.
static int hub_enumerate(uint8_t *buf, int root_port, int hub_slot, int hub_speed,
                         uint32_t route, int tt_slot, int tt_port,
                         uint32_t skip_mask, int first_time,
                         int *dport, int *dspeed) {
    if (first_time) {
        // Hub must be configured before its ports work.
        if (control_xfer(0x00, 9, 1, 0, 0, buf) < 0) { uart_puts("[HUB] set config failed\n"); return 0; }
    }
    // Class hub descriptor (type 0x29): bNbrPorts, wHubCharacteristics, pwr-good.
    if (control_xfer(0xA0, 6, (0x29 << 8), 0, 8, buf) < 0) { uart_puts("[HUB] get desc failed\n"); return 0; }
    int nports   = buf[2];
    int wHubChar = buf[3] | (buf[4] << 8);
    int ttt      = (wHubChar >> 5) & 3;                   // TT think time
    int pgood    = buf[5] * 2;                            // ms
    uart_dec("[HUB] ports ", nports);
    uart_hex("[HUB] wHubChar ", (uint32_t)wHubChar);

    if (first_time) {
        // Tell the xHC this slot is a hub (route split transactions through it):
        // Hub=1 (dword0 bit26), NumberOfPorts (dword1 24-31), TTT (dword2 16-17). Keep
        // the hub's existing route string and TT so deeper hubs stay reachable.
        uint32_t *icc = (uint32_t *)input_ctx;
        icc[0] = 0; icc[1] = (1u << 0);                   // evaluate slot context only
        uint32_t *slot = slot_ctx(input_ctx + ctx_size);
        slot[0] = (1u << 27) | ((uint32_t)hub_speed << 20) | (1u << 26) | (route & 0xfffff);
        slot[1] = (root_port << 16) | (nports << 24);
        slot[2] = (ttt << 16) | (tt_slot ? ((tt_slot & 0xff) | ((tt_port & 0xff) << 8)) : 0);
        if (run_command(pa(input_ctx), TRB_TYPE(TRB_CONFIGURE_EP) | (hub_slot << 24)) < 0)
            uart_puts("[HUB] set-hub config_ep failed (continuing)\n");

        // Power every downstream port.
        uart_dec("[HUB] pwr-good ms ", pgood);
        for (int p = 1; p <= nports; p++)
            control_xfer(0x23, 3, 8 /*PORT_POWER*/, p, 0, buf);
        usleep((pgood + 50) * 1000);
    }

    // Wait for a device to appear. On the first pass a downstream hub (e.g. a
    // monitor's built-in hub) can take a while to assert a connection after its
    // upstream port is powered, so poll for up to ~6 s. On a rescan the ports are
    // already powered, so scan just briefly and skip the already-claimed port.
    int max_tries = first_time ? 60 : 3;
    int dp = 0;
    for (int tries = 0; tries < max_tries && !dp; tries++) {
        for (int p = 1; p <= nports; p++) {
            if (skip_mask & (1u << p)) continue;
            if (control_xfer(0xA3, 0, 0, p, 4, buf) < 0) continue;       // GET_STATUS
            uint32_t st = buf[0] | (buf[1] << 8);
            uint32_t ch = buf[2] | (buf[3] << 8);
            if (tries % 10 == 0) {                                       // ~ every 1 s
                uart_dec("[HUB] poll p", p);
                uart_hex("  st", st); uart_hex("  ch", ch);
            }
            if (st & 1) {                                                // PORT_CONNECTION
                dp = p;
                uart_dec("[HUB] connected port ", p);
                uart_hex("[HUB]   status ", st);
                break;
            }
        }
        if (!dp) usleep(100000);                                         // 100 ms
    }
    if (!dp) { uart_puts("[HUB] no downstream device\n"); return 0; }

    // Reset the connected port until it comes up ENABLED. A cheap multi-tier hub
    // (e.g. the CrowView's internal hub) sometimes clears PORT_RESET without
    // enabling the port: status reads back as connect+power but with neither the
    // enable bit (bit1) nor a speed bit set. Proceeding then misreads the speed
    // (defaults to FS) and Address Device fails with a transaction error. So retry
    // the reset a few times, waiting for the enable bit, before giving up.
    uint32_t st = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        control_xfer(0x23, 3, 4 /*PORT_RESET*/, dp, 0, buf);
        for (int i = 0; i < 50; i++) {
            usleep(10000);
            if (control_xfer(0xA3, 0, 0, dp, 4, buf) < 0) continue;
            st = buf[0] | (buf[1] << 8);
            if (!(st & (1 << 4))) break;                  // PORT_RESET cleared
        }
        control_xfer(0x23, 1, 20 /*C_PORT_RESET*/, dp, 0, buf);
        usleep(20000);                                    // let the port settle
        control_xfer(0xA3, 0, 0, dp, 4, buf);
        st = buf[0] | (buf[1] << 8);
        if (st & (1 << 1)) break;                         // PORT_ENABLE set -> good
        uart_hex("[HUB]   reset retry, status ", st);
        usleep(50000);
    }
    uart_hex("[HUB]   post-reset ", st);
    *dport  = dp;
    *dspeed = (st & (1 << 10)) ? 3 : (st & (1 << 9)) ? 2 : 1;   // HS/LS/FS
    uart_dec("[HUB] device on port ", dp);
    uart_dec("[HUB]   speed ", *dspeed);
    return dp;
}

// Configure the interrupt IN endpoint of the device currently addressed as
// slot_id (its ep0_ring / input_ctx are the shared globals). Reads the config
// descriptor, finds the HID interrupt IN endpoint, issues SET_CONFIG +
// SET_PROTOCOL(boot) + Configure Endpoint, and fills *out with the polling state
// (but does NOT prime it yet - priming is deferred until every device is set up,
// so control transfers during a later device's enumeration are not confused by an
// already-armed interrupt endpoint). *out_proto gets the boot protocol
// (1=keyboard, 2=mouse). Returns 1 on success, 0 on failure.
static int setup_int_endpoint(int dev_speed, uint32_t dev_route, int root_port,
                              int dev_tt_slot, int dev_tt_port,
                              hid_ep_t *out, int *out_proto) {
    uint8_t *buf = dma_alloc(256, 64);
    if (control_xfer(0x80, 6, (2 << 8), 0, 9, buf) < 0) return 0;
    int tlen = buf[2] | (buf[3] << 8);
    if (tlen > 256) tlen = 256;
    if (control_xfer(0x80, 6, (2 << 8), 0, tlen, buf) < 0) return 0;

    int ep_addr = 0, ep_mps = 8, ep_interval = 8, in_hid = 0, if_proto = 0, proto = 0;
    int if_num = 0, hid_ifnum = 0;
    for (int p = 0; p + 2 <= tlen; ) {
        int blen = buf[p], btype = buf[p + 1];
        if (blen < 2) break;
        if (btype == 4) {                                      // interface
            in_hid   = (buf[p + 5] == 3);
            if_num   = buf[p + 2];                             // for SET_REPORT
            if_proto = buf[p + 7];
        } else if (btype == 5 && in_hid) {                     // endpoint
            if ((buf[p + 2] & 0x80) && (buf[p + 3] & 3) == 3) {
                ep_addr = buf[p + 2] & 0x0f;
                ep_mps  = (buf[p + 4] | (buf[p + 5] << 8)) & 0x7ff;
                ep_interval = buf[p + 6];
                proto = if_proto;
                hid_ifnum = if_num;
                break;
            }
        }
        p += blen;
    }
    if (!ep_addr) { uart_puts("[XHCI] no HID IN endpoint\n"); return 0; }
    uart_dec("[XHCI] HID EP ", ep_addr);
    uart_dec("[XHCI] HID proto ", proto);

    // SET_CONFIGURATION 1, SET_PROTOCOL boot(0), SET_IDLE(0) = report only on
    // change (indefinite). Boot keyboards need SET_IDLE(0) or they may follow a
    // default idle rate that suppresses key-change interrupt reports.
    if (control_xfer(0x00, 9, 1, 0, 0, buf) < 0) { uart_puts("[XHCI] SET_CONFIG failed\n"); return 0; }
    control_xfer(0x21, 0x0b, 0, 0, 0, buf);                    // SET_PROTOCOL (boot)
    control_xfer(0x21, 0x0a, 0, (uint16_t)hid_ifnum, 0, buf);  // SET_IDLE 0 (report on change)

    int dci  = ep_addr * 2 + 1;                                // IN endpoint DCI
    int rlen = ep_mps < 8 ? ep_mps : 8;                        // report size (kbd=8, mouse 3/4)
    epint_ring = dma_alloc(RING_TRBS * sizeof(trb_t), 64);
    epint_enq = 0; epint_cycle = TRB_CYCLE;
    report_buf = dma_alloc(8, 64);

    for (int i = 0; i < 33 * ctx_size; i++) input_ctx[i] = 0;
    uint32_t *icc = (uint32_t *)input_ctx;
    icc[1] = (1u << 0) | (1u << dci);                          // add slot + this EP
    // Rebuild the FULL slot context (A0 is set, so it is evaluated): keep the
    // device's route string / speed / TT, and bump Context Entries to this EP.
    uint32_t *slot = slot_ctx(input_ctx + ctx_size);
    slot[0] = (dci << 27) | ((uint32_t)dev_speed << 20) | (dev_route & 0xfffff);
    slot[1] = (root_port << 16);
    if (dev_tt_slot)
        slot[2] = (dev_tt_slot & 0xff) | ((dev_tt_port & 0xff) << 8);
    uint32_t *epi = ep_ctx(input_ctx + ctx_size, dci);
    // xHCI endpoint Interval: the endpoint is serviced every 2^Interval
    // microframes (125us each). For FULL/LOW-speed interrupt endpoints bInterval
    // is in 1ms frames (= 8 microframes), so Interval = floor(log2(bInterval*8)),
    // clamped to [3,10] (1ms..~16ms). The old code forced 1ms (interval=3) for
    // EVERY HID device; a full-speed mouse tolerates that, but a LOW-speed keyboard
    // behind the hub's transaction translator cannot be periodic-split-polled that
    // fast - it delivered exactly one report then went silent. Honour bInterval.
    int bint = ep_interval > 0 ? ep_interval : 8;             // frames; ~8ms default
    int microframes = bint * 8;
    int interval = 3;
    while (interval < 10 && (1 << (interval + 1)) <= microframes) interval++;
    epi[0] = ((uint32_t)interval << 16);
    epi[1] = (7u << 3) | (ep_mps << 16) | (3u << 1);          // EP type 7 = interrupt IN
    uint64_t itrp = pa(epint_ring) | 1;
    epi[2] = (uint32_t)itrp;
    epi[3] = (uint32_t)(itrp >> 32);
    // Low 16 bits = Average TRB Length; high 16 bits = Max ESIT Payload (Lo). The
    // Max ESIT Payload tells the scheduler how much bus time to RESERVE for this
    // periodic endpoint each interval; left at 0 the controller reserves nothing,
    // so a low-speed keyboard behind the hub TT gets serviced only intermittently
    // (a full-speed mouse usually squeaks through). Reserve one packet per ESIT.
    epi[4] = ep_mps | ((uint32_t)ep_mps << 16);              // avg TRB len | Max ESIT Payload

    if (run_command(pa(input_ctx), TRB_TYPE(TRB_CONFIGURE_EP) | (slot_id << 24)) < 0) {
        uart_puts("[XHCI] configure EP failed\n"); return 0;
    }

    out->ready = 1; out->slot = slot_id; out->dci = dci;
    out->ring = epint_ring; out->enq = epint_enq; out->cyc = epint_cycle;
    out->buf = report_buf; out->len = rlen;
    if (out_proto) *out_proto = proto;

    // Keep this device's EP0 if it is the keyboard. Setting the lock LEDs is a
    // control transfer made much later, by which time the globals below will
    // describe whatever was enumerated after it (typically the mouse).
    if (proto == 1) {
        kbd_ctrl.ready = 1;
        kbd_ctrl.slot  = slot_id;
        kbd_ctrl.iface = hid_ifnum;
        kbd_ctrl.ring  = ep0_ring;
        kbd_ctrl.enq   = ep0_enq;
        kbd_ctrl.cyc   = ep0_cycle;
        kbd_ctrl.led   = dma_alloc(4, 64);
    }
    return 1;
}

// Push the lock LEDs to the keyboard when they change. control_xfer works on the
// global EP0 ring, so the keyboard's is swapped in for the duration and the
// caller's put back - including the ring's advanced enqueue pointer, which must
// be carried forward or the next transfer would reuse a TRB the controller owns.
static void xhci_sync_leds(void) {
    if (!kbd_ctrl.ready || !hid_leds_dirty()) return;

    trb_t   *save_ring = ep0_ring;
    uint32_t save_enq  = ep0_enq, save_cyc = ep0_cycle;
    int      save_slot = slot_id;

    ep0_ring = kbd_ctrl.ring; ep0_enq = kbd_ctrl.enq; ep0_cycle = kbd_ctrl.cyc;
    slot_id  = kbd_ctrl.slot;

    kbd_ctrl.led[0] = (uint8_t)hid_leds();
    // SET_REPORT (class, interface): wValue = report type 2 (Output) << 8 | id 0
    int r = control_xfer(0x21, 0x09, 0x0200, (uint16_t)kbd_ctrl.iface, 1, kbd_ctrl.led);

    kbd_ctrl.enq = ep0_enq; kbd_ctrl.cyc = ep0_cycle;
    ep0_ring = save_ring; ep0_enq = save_enq; ep0_cycle = save_cyc;
    slot_id  = save_slot;

    if (r == 0) hid_leds_ack();            // a failure simply retries next poll
}

// Route a freshly configured HID endpoint to the keyboard or mouse slot by its
// boot protocol: 1 = keyboard, 2 = mouse. Anything else (notably a touchpad that
// only implements the HID report protocol and reports bInterfaceProtocol 0) is
// NOT a boot device, so we ignore it rather than let it clobber a real keyboard
// or mouse slot (the CrowView touchpad reports proto 0 and used to overwrite the
// keyboard). Returns 1 if claimed.
static int claim_hid(hid_ep_t *ep, int proto) {
    if (proto == 1) { kbd_ep_st = *ep; uart_puts("[XHCI] keyboard found\n"); return 1; }
    if (proto == 2) { mou_ep_st = *ep; uart_puts("[XHCI] mouse found\n");    return 1; }
    uart_dec("[XHCI] ignoring non-boot HID, proto ", proto);
    return 0;
}

// Prime an interrupt endpoint with `n` outstanding transfers. Several matter for
// the low-speed keyboard: its initial report completes during the idle gap before
// the REPL starts pumping, and if that drains the ring to EMPTY the VL805 does not
// reliably resume polling when a single TRB is later re-queued - the endpoint goes
// silent (exactly the "one completion then nothing" we see). Keeping the ring
// non-empty makes the controller keep servicing it. All TRBs share e->buf; reports
// are milliseconds apart and we pump continuously, so we never miss one.
static void hid_prime_n(hid_ep_t *e, int n) {
    for (int i = 0; i < n; i++)
        ring_push(e->ring, &e->enq, &e->cyc, pa(e->buf), e->len,
                  TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    ring_db(e->slot, e->dci);
}
static void hid_prime(hid_ep_t *e) { hid_prime_n(e, 1); }

// Re-arm an interrupt endpoint after a completed transfer.
static void hid_rearm(hid_ep_t *e) {
    ring_push(e->ring, &e->enq, &e->cyc, pa(e->buf), e->len,
              TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    ring_db(e->slot, e->dci);
}

static void key_push(int c) {
    if (!c) return;
    int nt = (key_tail + 1) % KEY_FIFO_N;
    if (nt == key_head) return;                  // full: drop oldest-avoiding, just drop new
    key_fifo[key_tail] = c; key_tail = nt;
}
static int key_pop(void) {
    if (key_head == key_tail) return 0;
    int c = key_fifo[key_head];
    key_head = (key_head + 1) % KEY_FIFO_N;
    return c;
}

// Drain every currently-available transfer event, routing each completion to the
// keyboard or mouse interrupt endpoint by (slot, DCI) - both post to the single
// shared event ring, so this demux is what lets two HID devices coexist. Decoded
// keys go to key_fifo; mouse movement accumulates in mou_*. Also re-rings idle
// doorbells to survive the VL805 lost-doorbell race (see run_command).
// Route one transfer-event completion to the keyboard or mouse interrupt
// endpoint by (slot, DCI), decoding + re-arming it. Shared by xhci_pump and the
// bulk-transfer wait, so a HID report that lands mid-bulk-transfer is not lost.
static void handle_hid_event(int sid, int eid, int cc) {
    if (kbd_ep_st.ready && sid == kbd_ep_st.slot && eid == kbd_ep_st.dci) {
        if (cc == 1 || cc == 13) key_push(hid_report_key(kbd_ep_st.buf, kbd_prev));
        hid_rearm(&kbd_ep_st);
    } else if (mou_ep_st.ready && sid == mou_ep_st.slot && eid == mou_ep_st.dci) {
        if (cc == 1 || cc == 13) {
            int b, dx, dy, w;
            if (hid_mouse_decode(mou_ep_st.buf, mou_ep_st.len, &b, &dx, &dy, &w)) {
                mou_btn = b; mou_dx += dx; mou_dy += dy; mou_wheel += w; mou_have = 1;
            }
        }
        hid_rearm(&mou_ep_st);
    }
}

static void xhci_pump(void) {
    static uint32_t idle = 0;
    trb_t ev;
    int got = 0;
    while (next_event(&ev, 1)) {
        if (TRB_GET_TYPE(ev.control) != TRB_TRANSFER_EVENT) continue;
        got = 1;
        int sid = (ev.control >> 24) & 0xff;
        int eid = (ev.control >> 16) & 0x1f;
        int cc  = COMP_CODE(ev.status);
        handle_hid_event(sid, eid, cc);
    }
    if (got) idle = 0;
    else if (++idle >= 64) {
        idle = 0;
        if (kbd_ep_st.ready) ring_db(kbd_ep_st.slot, kbd_ep_st.dci);
        if (mou_ep_st.ready) ring_db(mou_ep_st.slot, mou_ep_st.dci);
    }
}

// ===========================================================================
// Generic device table + recursive enumeration (Phase 2). Every enumerated
// device is recorded with its topology and first-interface class; each device's
// EP0 pipe + contexts are snapshotted so control/bulk transfers can target it
// later (the globals only ever describe the most-recently-addressed device).
// ===========================================================================
#define XHCI_MAX_DEV 12    // hub + keyboard + mouse + several sticks (freed slots reused)

static xhci_dev g_devs[XHCI_MAX_DEV];
static int      g_dev_n;

static int g_enum_hub_port;   // downstream hub port being enumerated now (0 = root)
// Onboard hub topology, captured when it is first scanned, so a hot-plug rescan
// can re-drive its ports without re-walking the whole tree.
static int g_ihub_valid, g_ihub_slot, g_ihub_speed, g_ihub_root_port,
           g_ihub_tt_slot, g_ihub_tt_port;
static uint32_t g_ihub_route;

// Per-device private pipe state, parallel to g_devs.
typedef struct {
    trb_t   *ep0_ring; uint32_t ep0_enq, ep0_cyc;
    uint8_t *input_ctx, *dev_ctx;
    trb_t   *in_ring;  uint32_t in_enq,  in_cyc;  int in_dci;
    trb_t   *out_ring; uint32_t out_enq, out_cyc; int out_dci;
} dev_priv;
static dev_priv g_priv[XHCI_MAX_DEV];

static int ep_addr_dci(int ep_addr) { return (ep_addr & 0x0f) * 2 + ((ep_addr & 0x80) ? 1 : 0); }

// Record the currently-addressed device (globals describe it) with the given
// topology + interface class. Snapshots its EP0 pipe and contexts.
static int record_dev(int root_port, uint32_t route, int speed, int tt_slot, int tt_port,
                      int ifnum, int cls, int subcls, int proto) {
    int i = -1;                                   // reuse a freed (unplugged) slot first
    for (int k = 0; k < g_dev_n; k++) if (!g_devs[k].valid) { i = k; break; }
    if (i < 0) { if (g_dev_n >= XHCI_MAX_DEV) return -1; i = g_dev_n++; }
    xhci_dev *d = &g_devs[i];
    d->slot = slot_id; d->speed = speed; d->root_port = root_port; d->route = route;
    d->tt_slot = tt_slot; d->tt_port = tt_port; d->ifnum = ifnum;
    d->cls = (uint8_t)cls; d->subcls = (uint8_t)subcls; d->proto = (uint8_t)proto;
    d->hub_port = g_enum_hub_port;
    d->bulk_in = d->bulk_out = d->bulk_mps = 0;
    d->valid = 1;
    dev_priv *p = &g_priv[i];
    p->ep0_ring = ep0_ring; p->ep0_enq = ep0_enq; p->ep0_cyc = ep0_cycle;
    p->input_ctx = input_ctx; p->dev_ctx = dev_ctx;
    p->in_ring = 0; p->out_ring = 0; p->in_dci = 0; p->out_dci = 0;
    uart_dec("[USB] dev slot=", slot_id);
    uart_hex("[USB]   class ", ((uint32_t)cls << 16) | ((uint32_t)subcls << 8) | proto);
    return i;
}

static int find_priv(int slot) {
    for (int i = 0; i < g_dev_n; i++) if (g_devs[i].valid && g_devs[i].slot == slot) return i;
    return -1;
}

// Read the config descriptor of the currently-addressed device and extract its
// FIRST interface's class/subclass/protocol + interface number. For a
// mass-storage interface (class 8) also find the bulk IN/OUT endpoint addresses
// and max packet size. Returns 0 on success (fields filled), <0 on error.
static int probe_iface(int *cls, int *subcls, int *proto, int *ifnum,
                       int *bulk_in, int *bulk_out, int *bulk_mps) {
    uint8_t *buf = dma_alloc(256, 64);
    if (control_xfer(0x80, 6, (2 << 8), 0, 9, buf) < 0) return -1;       // config desc header
    int tlen = buf[2] | (buf[3] << 8);
    if (tlen > 256) tlen = 256;
    if (control_xfer(0x80, 6, (2 << 8), 0, tlen, buf) < 0) return -1;

    int c = 0, s = 0, pr = 0, ifn = 0, have_if = 0;
    int bi = 0, bo = 0, bm = 0, in_msc = 0;
    for (int p = 0; p + 2 <= tlen; ) {
        int blen = buf[p], btype = buf[p + 1];
        if (blen < 2) break;
        if (btype == 4) {                              // interface descriptor
            if (!have_if) {                            // first interface wins
                ifn = buf[p + 2]; c = buf[p + 5]; s = buf[p + 6]; pr = buf[p + 7];
                have_if = 1;
            }
            in_msc = (buf[p + 5] == 0x08);             // scan THIS interface's bulk eps
        } else if (btype == 5 && in_msc) {             // endpoint descriptor
            int eaddr = buf[p + 2];
            int etype = buf[p + 3] & 3;                // 2 = bulk
            int emps  = (buf[p + 4] | (buf[p + 5] << 8)) & 0x7ff;
            if (etype == 2) {
                if (eaddr & 0x80) bi = eaddr; else bo = eaddr;
                if (!bm) bm = emps;
            }
        }
        p += blen;
    }
    if (!have_if) return -1;
    if (cls)      *cls = c;
    if (subcls)   *subcls = s;
    if (proto)    *proto = pr;
    if (ifnum)    *ifnum = ifn;
    if (bulk_in)  *bulk_in = bi;
    if (bulk_out) *bulk_out = bo;
    if (bulk_mps) *bulk_mps = bm;
    return 0;
}

// Handle a just-addressed leaf device (globals describe it). Record it, and if it
// is a HID boot device set up + claim its interrupt endpoint (keyboard/mouse), as
// before. Mass storage is left addressed for the MSC layer (Phase 3).
static void handle_leaf(int root_port, uint32_t route, int speed, int tt_slot, int tt_port) {
    int cls = 0, subcls = 0, proto = 0, ifn = 0, bi = 0, bo = 0, bm = 0;
    if (probe_iface(&cls, &subcls, &proto, &ifn, &bi, &bo, &bm) < 0) {
        uart_puts("[USB] probe_iface failed\n"); return;
    }
    int di = record_dev(root_port, route, speed, tt_slot, tt_port, ifn, cls, subcls, proto);
    if (cls == 0x08 && di >= 0) {                       // remember bulk eps for the MSC layer
        g_devs[di].bulk_in  = bi;
        g_devs[di].bulk_out = bo;
        g_devs[di].bulk_mps = bm;
        uart_hex("[USB]   MSC subcls/proto ", ((uint32_t)subcls << 8) | proto);
        uart_dec("[USB]   bulk IN  ", bi);
        uart_dec("[USB]   bulk OUT ", bo);
        uart_dec("[USB]   bulk MPS ", bm);
    }
    if (cls == 3) {                                    // HID: keyboard / mouse
        hid_ep_t ep; int hproto = 0;
        if (setup_int_endpoint(speed, route, root_port, tt_slot, tt_port, &ep, &hproto))
            claim_hid(&ep, hproto);
    }
}

static void scan_hub(int hub_slot, int hub_speed, uint32_t hub_route,
                     int hub_tt_slot, int hub_tt_port, int root_port, int depth,
                     uint8_t *buf);

// Address + descend into the device the globals point at (buf holds its device
// descriptor). A hub is scanned recursively; a leaf is handled.
static void handle_device(int root_port, uint32_t route, int speed,
                          int tt_slot, int tt_port, int depth, uint8_t *buf) {
    if (buf[4] == 0x09) {                              // hub
        record_dev(root_port, route, speed, tt_slot, tt_port, 0, 0x09, 0, 0);
        scan_hub(slot_id, speed, route, tt_slot, tt_port, root_port, depth, buf);
    } else {
        handle_leaf(root_port, route, speed, tt_slot, tt_port);
    }
}

// Address + descend into the child on downstream port `dp` of the hub whose EP0
// is currently selected in the globals. Addressing the child moves the globals,
// so snapshot the hub's EP0 and re-select it afterwards. Shared by the boot scan
// and the hot-plug rescan.
static void enum_child(int hub_slot, int hub_speed, uint32_t hub_route,
                       int hub_tt_slot, int hub_tt_port, int root_port, int depth,
                       int dp, int dspeed, uint8_t *buf) {
    trb_t   *hub_ring = ep0_ring; uint32_t he = ep0_enq, hc = ep0_cycle;
    int      hslot = slot_id;
    uint32_t croute = hub_route | ((uint32_t)(dp & 0xf) << (4 * depth));
    int ctt_slot = hub_tt_slot, ctt_port = hub_tt_port;
    if (dspeed < 3) { ctt_slot = hub_slot; ctt_port = dp; }   // FS/LS -> parent is the TT
    g_enum_hub_port = dp;
    int cslot = address_device_on(root_port, croute, dspeed, ctt_slot, ctt_port);
    if (cslot && get_device_descriptor(buf, dspeed) >= 0)
        handle_device(root_port, croute, dspeed, ctt_slot, ctt_port, depth + 1, buf);
    g_enum_hub_port = 0;
    ep0_ring = hub_ring; ep0_enq = he; ep0_cycle = hc; slot_id = hslot;  // re-select hub
}

// Enumerate every connected downstream port of a hub (globals point at the hub).
static void scan_hub(int hub_slot, int hub_speed, uint32_t hub_route,
                     int hub_tt_slot, int hub_tt_port, int root_port, int depth,
                     uint8_t *buf) {
    if (depth >= 5) { uart_puts("[USB] hub chain too deep\n"); return; }
    if (depth == 0) {                                 // remember the onboard hub for rescans
        g_ihub_valid = 1; g_ihub_slot = hub_slot; g_ihub_speed = hub_speed;
        g_ihub_route = hub_route; g_ihub_tt_slot = hub_tt_slot; g_ihub_tt_port = hub_tt_port;
        g_ihub_root_port = root_port;
    }
    uint32_t skip = 0;
    int first_time = 1;
    for (;;) {
        int dp = 0, dspeed = 0;
        if (!hub_enumerate(buf, root_port, hub_slot, hub_speed, hub_route,
                           hub_tt_slot, hub_tt_port, skip, first_time, &dp, &dspeed))
            break;                                     // no more connected ports
        first_time = 0;
        skip |= (1u << dp);
        enum_child(hub_slot, hub_speed, hub_route, hub_tt_slot, hub_tt_port,
                   root_port, depth, dp, dspeed, buf);
    }
    // The hub's EP0 ring advanced through every port poll/reset above, but its
    // saved pipe (g_priv) was snapshotted back in record_dev, before all that.
    // Write the final position back so later control transfers to the hub - above
    // all the hot-plug rescan - continue from here instead of a stale ring slot
    // the controller has already passed (which would time out and look like every
    // device vanished).
    int hpi = find_priv(hub_slot);
    if (hpi >= 0) { g_priv[hpi].ep0_enq = ep0_enq; g_priv[hpi].ep0_cyc = ep0_cycle; }
}

int xhci_kbd_init(uintptr_t mmio_base) {
    cap_base = mmio_base;
    arena_off = 0;
    g_dev_n = 0;
    kbd_ep_st.ready = 0;
    mou_ep_st.ready = 0;
    key_head = key_tail = 0;
    mou_have = 0; mou_dx = 0; mou_dy = 0; mou_wheel = 0;

    // The arena must be non-cacheable so the controller sees our writes.
    mmu_set_noncached(pa(arena), ARENA_SIZE);

    int maxports = xhci_start();
    if (maxports < 0) return 0;

    // Enumerate EVERY connected root port (not just the first). On the Pi 4 the
    // USB-A ports hang off the onboard hub on one root port; handle_device
    // descends it recursively, recording every device (keyboard, mouse, and a
    // mass-storage stick) into g_devs. HID boot devices are set up + claimed;
    // mass storage is left addressed for the MSC layer to configure.
    uint8_t *buf = dma_alloc(256, 64);
    for (int p = 1; p <= maxports; p++) {
        uint32_t sc = RD32(op_base + OP_PORTSC(p));
        uart_dec("[XHCI] port ", p);
        uart_hex("[XHCI]   PORTSC: ", sc);              // speed = bits [13:10]
        if (!(sc & PORTSC_CCS)) continue;               // nothing connected
        if (!port_reset(p)) { uart_puts("[XHCI] port reset failed\n"); continue; }
        int speed = (RD32(op_base + OP_PORTSC(p)) >> 10) & 0xf;
        uart_dec("[XHCI] dev speed ", speed);
        int rslot = address_device_on(p, 0, speed, 0, 0);
        if (!rslot) continue;
        if (get_device_descriptor(buf, speed) < 0) {
            uart_puts("[XHCI] GET_DESCRIPTOR failed\n"); continue;
        }
        uart_hex("[XHCI] VID ", buf[8] | (buf[9] << 8));
        uart_hex("[XHCI] PID ", buf[10] | (buf[11] << 8));
        uart_hex("[XHCI] bDeviceClass ", buf[4]);       // 0x09 = hub, 0x00 = per-interface
        handle_device(p, 0, speed, 0, 0, 0, buf);
    }

    // All devices are configured; NOW arm the HID interrupt endpoints. Priming
    // earlier would let an interrupt completion land on the shared event ring
    // mid-enumeration and be mistaken for a control-transfer completion.
    if (kbd_ep_st.ready) hid_prime_n(&kbd_ep_st, 8);   // keep the ring non-empty (see hid_prime_n)
    if (mou_ep_st.ready) hid_prime(&mou_ep_st);         // mouse never idles its ring; one is fine

    if (mou_ep_st.ready) uart_puts("[XHCI] mouse ready\n");
    if (kbd_ep_st.ready) uart_puts("[XHCI] keyboard ready\n");
    return kbd_ep_st.ready;
}

int xhci_kbd_getchar(void) {
    if (!kbd_ep_st.ready) return 0;
    xhci_pump();
    xhci_sync_leds();                     // a lock key may have toggled
    return key_pop();
}

// --- mouse (real hardware) --------------------------------------------------
int xhci_mouse_present(void) { return mou_ep_st.ready; }

int xhci_mouse_poll(int *btn, int *dx, int *dy, int *wheel) {
    if (!mou_ep_st.ready) return 0;
    xhci_pump();
    if (!mou_have) return 0;
    if (btn)   *btn   = mou_btn;
    if (dx)    *dx    = mou_dx;
    if (dy)    *dy    = mou_dy;
    if (wheel) *wheel = mou_wheel;
    mou_have = 0; mou_dx = 0; mou_dy = 0; mou_wheel = 0;
    return 1;
}

// ===========================================================================
// Generic control + bulk transport (Phase 2). See xhci.h.
// ===========================================================================
#define TRB_RESET_ENDPOINT   14
#define TRB_SET_TR_DEQUEUE   16

void *xhci_dma_alloc(unsigned size, unsigned align) { return dma_alloc(size, align); }

int       xhci_dev_count(void)  { return g_dev_n; }
xhci_dev *xhci_dev_get(int i)   { if (i < 0 || i >= g_dev_n) return 0; return &g_devs[i]; }

// Control transfer to an arbitrary device: swap its EP0 pipe into the globals,
// run control_xfer, save the advanced enqueue pointer back.
int xhci_control(int slot, unsigned char bmReqType, unsigned char bReq,
                 unsigned short wValue, unsigned short wIndex,
                 unsigned short wLength, void *buf) {
    int i = find_priv(slot);
    if (i < 0) return -1;
    dev_priv *p = &g_priv[i];
    trb_t *sr = ep0_ring; uint32_t se = ep0_enq, sc = ep0_cycle; int ss = slot_id;
    ep0_ring = p->ep0_ring; ep0_enq = p->ep0_enq; ep0_cycle = p->ep0_cyc; slot_id = slot;
    int r = control_xfer(bmReqType, bReq, wValue, wIndex, wLength, buf);
    p->ep0_enq = ep0_enq; p->ep0_cyc = ep0_cycle;              // carry the ring forward
    ep0_ring = sr; ep0_enq = se; ep0_cycle = sc; slot_id = ss;
    return r;
}

int xhci_bulk_config(int slot, int ep_in, int ep_out, int mps) {
    int i = find_priv(slot);
    if (i < 0) return -1;
    xhci_dev *d = &g_devs[i];
    dev_priv *p = &g_priv[i];
    int in_dci  = ep_addr_dci(ep_in);
    int out_dci = ep_addr_dci(ep_out);
    int max_dci = in_dci > out_dci ? in_dci : out_dci;

    p->in_ring  = dma_alloc(RING_TRBS * sizeof(trb_t), 64);
    p->out_ring = dma_alloc(RING_TRBS * sizeof(trb_t), 64);
    p->in_enq = 0;  p->in_cyc = TRB_CYCLE;  p->in_dci = in_dci;
    p->out_enq = 0; p->out_cyc = TRB_CYCLE; p->out_dci = out_dci;

    uint8_t *ic = p->input_ctx;
    for (int k = 0; k < 33 * ctx_size; k++) ic[k] = 0;
    uint32_t *icc = (uint32_t *)ic;
    icc[1] = (1u << 0) | (1u << in_dci) | (1u << out_dci);     // slot + both endpoints
    uint32_t *slotc = slot_ctx(ic + ctx_size);
    slotc[0] = ((uint32_t)max_dci << 27) | ((uint32_t)d->speed << 20) | (d->route & 0xfffff);
    slotc[1] = ((uint32_t)d->root_port << 16);
    if (d->tt_slot) slotc[2] = (d->tt_slot & 0xff) | ((d->tt_port & 0xff) << 8);
    // Bulk IN endpoint: EP Type 6. Bulk OUT: EP Type 2. Interval irrelevant (0).
    uint32_t *epi = ep_ctx(ic + ctx_size, in_dci);
    epi[1] = (6u << 3) | ((uint32_t)mps << 16) | (3u << 1);    // EP type + MPS + CErr=3
    { uint64_t r = pa(p->in_ring) | 1; epi[2] = (uint32_t)r; epi[3] = (uint32_t)(r >> 32); }
    epi[4] = mps;                                             // average TRB length
    uint32_t *epo = ep_ctx(ic + ctx_size, out_dci);
    epo[1] = (2u << 3) | ((uint32_t)mps << 16) | (3u << 1);
    { uint64_t r = pa(p->out_ring) | 1; epo[2] = (uint32_t)r; epo[3] = (uint32_t)(r >> 32); }
    epo[4] = mps;

    if (run_command(pa(ic), TRB_TYPE(TRB_CONFIGURE_EP) | (slot << 24)) < 0) {
        uart_puts("[XHCI] bulk configure EP failed\n"); return -1;
    }
    return 0;
}

// Wait for the Transfer Event on (slot, dci), routing HID completions so a report
// arriving mid-transfer is not lost. Returns the completion code and fills
// *residue (bytes NOT transferred); -1 on timeout.
static int bulk_wait(int slot, int dci, int *residue, int timeout_ms) {
    trb_t ev;
    int slices = timeout_ms / 50 + 1;
    for (int t = 0; t < slices; t++) {
        if (!next_event(&ev, 50)) { ring_db(slot, dci); continue; }
        if (TRB_GET_TYPE(ev.control) != TRB_TRANSFER_EVENT) continue;
        int sid = (ev.control >> 24) & 0xff;
        int eid = (ev.control >> 16) & 0x1f;
        int cc  = COMP_CODE(ev.status);
        if (sid == slot && eid == dci) {
            if (residue) *residue = ev.status & 0xffffff;
            return cc;
        }
        handle_hid_event(sid, eid, cc);                      // not ours: keep HID alive
    }
    return -1;
}

// One Normal-TRB TD. Caps at 64 KiB (the SCSI layer loops for more). Completion
// codes: 1 Success, 13 Short Packet (success, shorter), 6 Stall. -> bytes, or
// <0: -1 timeout/error, -2 stall.
static int bulk_xfer(int slot, void *buf, int len, int in, int timeout_ms) {
    int i = find_priv(slot);
    if (i < 0) return -1;
    dev_priv *p = &g_priv[i];
    trb_t *ring; uint32_t *enq; uint32_t *cyc; int dci;
    if (in)  { ring = p->in_ring;  enq = &p->in_enq;  cyc = &p->in_cyc;  dci = p->in_dci;  }
    else     { ring = p->out_ring; enq = &p->out_enq; cyc = &p->out_cyc; dci = p->out_dci; }
    if (!ring) return -1;

    ring_push(ring, enq, cyc, pa(buf), (uint32_t)len, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    dsb();
    ring_db(slot, dci);

    int resid = 0;
    int cc = bulk_wait(slot, dci, &resid, timeout_ms);
    if (cc < 0) return -1;                                    // timeout
    if (cc == 6) return -2;                                   // stall
    if (cc != 1 && cc != 13) { uart_hex("[XHCI] bulk cc ", cc); return -1; }
    return len - resid;                                       // short packet -> fewer bytes
}

int xhci_bulk_in (int slot, void *buf, int len, int timeout_ms)       { return bulk_xfer(slot, buf, len, 1, timeout_ms); }
int xhci_bulk_out(int slot, const void *buf, int len, int timeout_ms) { return bulk_xfer(slot, (void *)buf, len, 0, timeout_ms); }

int xhci_ep_reset(int slot, int ep_addr) {
    int i = find_priv(slot);
    if (i < 0) return -1;
    dev_priv *p = &g_priv[i];
    int dci = ep_addr_dci(ep_addr);
    trb_t *ring; uint32_t enq, cyc;
    if      (dci == p->in_dci)  { ring = p->in_ring;  enq = p->in_enq;  cyc = p->in_cyc;  }
    else if (dci == p->out_dci) { ring = p->out_ring; enq = p->out_enq; cyc = p->out_cyc; }
    else return -1;
    // Reset Endpoint (Halted -> Stopped), reset the ring dequeue to where we are,
    // then clear the device-side HALT feature.
    run_command(0, TRB_TYPE(TRB_RESET_ENDPOINT) | (dci << 16) | (slot << 24));
    uint64_t dq = pa(&ring[enq]) | ((cyc & TRB_CYCLE) ? 1 : 0);
    run_command(dq, TRB_TYPE(TRB_SET_TR_DEQUEUE) | (dci << 16) | (slot << 24));
    xhci_control(slot, 0x02, 1 /*CLEAR_FEATURE*/, 0 /*ENDPOINT_HALT*/, (uint16_t)ep_addr, 0, 0);
    return 0;
}

// --- hot-plug rescan --------------------------------------------------------
#define TRB_DISABLE_SLOT 10

static void xhci_disable_slot(int slot) {
    run_command(0, TRB_TYPE(TRB_DISABLE_SLOT) | (slot << 24));
}

int xhci_slot_valid(int slot) {
    for (int i = 0; i < g_dev_n; i++)
        if (g_devs[i].valid && g_devs[i].slot == slot) return 1;
    return 0;
}

// Re-poll the onboard hub's downstream ports (via the hub's EP0), enumerate any
// newly-connected device and free any that vanished. Reuses the boot enumeration
// path (hub_enumerate + enum_child). Bitmask: 1 added, 2 removed. See xhci.h.
int xhci_rescan(void) {
    if (!g_ihub_valid) return 0;
    int hidx = find_priv(g_ihub_slot);
    if (hidx < 0) return 0;
    static uint8_t *rbuf;
    if (!rbuf) rbuf = dma_alloc(256, 64);
    uint8_t *buf = rbuf;
    int changed = 0;

    if (xhci_control(g_ihub_slot, 0xA0, 6, (0x29 << 8), 0, 8, buf) < 0) return 0;  // hub descriptor
    int nports = buf[2];
    if (nports > 15) nports = 15;

    uint32_t connected = 0;
    for (int p = 1; p <= nports; p++)
        if (xhci_control(g_ihub_slot, 0xA3, 0, 0, p, 4, buf) >= 0 && (buf[0] & 1))
            connected |= (1u << p);                                              // PORT_CONNECTION

    // Removals: a known hub child whose port no longer reports a connection.
    for (int i = 0; i < g_dev_n; i++) {
        xhci_dev *d = &g_devs[i];
        if (!d->valid || !d->hub_port || d->slot == g_ihub_slot) continue;
        if (!(connected & (1u << d->hub_port))) {
            uart_dec("[USB] hotplug: REMOVED, hub port ", (uint32_t)d->hub_port);
            xhci_disable_slot(d->slot);
            d->valid = 0;
            changed |= 2;
        }
    }

    // Additions: a connected port with no known child -> enumerate it.
    uint32_t known = 0;
    for (int i = 0; i < g_dev_n; i++)
        if (g_devs[i].valid && g_devs[i].hub_port) known |= (1u << g_devs[i].hub_port);
    for (int p = 1; p <= nports; p++) {
        if (!(connected & (1u << p)) || (known & (1u << p))) continue;
        uart_dec("[USB] hotplug: NEW device, hub port ", (uint32_t)p);
        dev_priv *hp = &g_priv[hidx];                       // select the hub's EP0
        trb_t *sr = ep0_ring; uint32_t se = ep0_enq, sc = ep0_cycle; int ss = slot_id;
        uint8_t *si = input_ctx, *sd = dev_ctx;
        ep0_ring = hp->ep0_ring; ep0_enq = hp->ep0_enq; ep0_cycle = hp->ep0_cyc;
        slot_id = g_ihub_slot; input_ctx = hp->input_ctx; dev_ctx = hp->dev_ctx;

        int dp = 0, dspeed = 0;
        if (hub_enumerate(buf, g_ihub_root_port, g_ihub_slot, g_ihub_speed, g_ihub_route,
                          g_ihub_tt_slot, g_ihub_tt_port, ~(1u << p), 0, &dp, &dspeed) && dp == p) {
            enum_child(g_ihub_slot, g_ihub_speed, g_ihub_route, g_ihub_tt_slot, g_ihub_tt_port,
                       g_ihub_root_port, 0, dp, dspeed, buf);
            changed |= 1;
        }
        hp->ep0_enq = ep0_enq; hp->ep0_cyc = ep0_cycle;     // carry the hub ring forward
        ep0_ring = sr; ep0_enq = se; ep0_cycle = sc; slot_id = ss;
        input_ctx = si; dev_ctx = sd;
        known |= (1u << p);
    }
    return changed;
}

void xhci_selftest(void) {
    uart_dec("[USB] selftest: devices ", g_dev_n);
    for (int i = 0; i < g_dev_n; i++) {
        xhci_dev *d = &g_devs[i];
        if (!d->valid) continue;
        uart_dec("[USB] dev slot=", d->slot);
        uart_hex("[USB]   class ", ((uint32_t)d->cls << 16) | ((uint32_t)d->subcls << 8) | d->proto);
        if (d->cls == 0x08 && d->subcls == 0x06 && d->proto == 0x50)
            uart_puts("[USB]   -> BOT/SCSI mass storage (ready for MSC)\n");
    }
}
