#include "pcie.h"
#include "uart.h"
#include "mmu.h"
#include "mailbox.h"

// ---------------------------------------------------------------------------
// BCM2711 PCIe root-complex bring-up + VL805 (VIA) xHCI discovery.
//
// Sequence follows the documented Broadcom STB PCIe init (cf. Linux
// pcie-brcmstb and U-Boot pcie_brcmstb). This only runs on real Pi 4 hardware;
// it is heavily UART-logged because it cannot be exercised under QEMU.
//
// Address plan:
//   - controller registers : 0xFD500000 (Device-mapped peripheral region)
//   - config space window   : controller + 0x8000 (downstream), index @ +0x9000
//   - outbound MMIO window   : CPU 0x6_00000000 -> PCIe 0xC0000000 (VL805 BAR0)
// ---------------------------------------------------------------------------

#define PCIE_BASE        0xFD500000UL
#define REG(off)         (*(volatile uint32_t *)(PCIE_BASE + (off)))

// --- controller registers ---------------------------------------------------
#define MISC_MISC_CTRL                 0x4008
#define MISC_CPU_2_PCIE_MEM_WIN0_LO    0x400c
#define MISC_CPU_2_PCIE_MEM_WIN0_HI    0x4010
#define MISC_RC_BAR1_CONFIG_LO         0x402c
#define MISC_RC_BAR2_CONFIG_LO         0x4034
#define MISC_RC_BAR2_CONFIG_HI         0x4038
#define MISC_RC_BAR3_CONFIG_LO         0x403c
#define MISC_PCIE_CTRL                 0x4064
#define MISC_PCIE_STATUS               0x4068
#define MISC_REVISION                  0x406c
#define MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT 0x4070
#define MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI    0x4080
#define MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI   0x4084
#define MISC_HARD_PCIE_HARD_DEBUG      0x4204
#define EXT_CFG_DATA                   0x8000
#define EXT_CFG_INDEX                  0x9000
#define RGR1_SW_INIT_1                 0x9210

#define RGR1_INIT_GENERIC_MASK         0x2     // bridge soft reset
#define RGR1_PERST_MASK                0x1     // endpoint PERST#

#define STATUS_PHYLINKUP               0x10
#define STATUS_DL_ACTIVE               0x20

// --- outbound window / VL805 location ---------------------------------------
#define OUTBOUND_CPU   0x600000000ULL    // CPU view of VL805 MMIO
#define OUTBOUND_PCIE  0xC0000000UL      // PCIe bus address it maps to
#define OUTBOUND_SIZE  0x40000000UL      // 1 GiB window

#define VL805_BUS  1
#define VL805_DEV  0
#define VL805_FN   0
#define VL805_ID   0x34831106u           // VIA VL805: device 0x3483, vendor 0x1106

static void usleep(uint32_t us) {
    volatile uint32_t *clo = (volatile uint32_t *)0xFE003004UL;
    uint32_t t = *clo;
    while (*clo - t < us) { }
}

// --- config-space access ----------------------------------------------------
// Bus 0 is the root complex itself (registers at PCIE_BASE). Downstream devices
// (bus >= 1) are reached through the EXT_CFG window.
static volatile uint32_t *cfg_addr(int bus, int dev, int fn, int reg) {
    if (bus == 0)
        return (volatile uint32_t *)(PCIE_BASE + (reg & 0xffc));
    int devfn = (dev << 3) | fn;
    REG(EXT_CFG_INDEX) = (bus << 20) | (devfn << 12);
    return (volatile uint32_t *)(PCIE_BASE + EXT_CFG_DATA + (reg & 0xffc));
}

static uint32_t cfg_read(int bus, int dev, int fn, int reg) {
    return *cfg_addr(bus, dev, fn, reg);
}
static void cfg_write(int bus, int dev, int fn, int reg, uint32_t val) {
    *cfg_addr(bus, dev, fn, reg) = val;
}

static void bridge_sw_init(int assert) {
    uint32_t v = REG(RGR1_SW_INIT_1);
    if (assert) v |= RGR1_INIT_GENERIC_MASK;
    else        v &= ~RGR1_INIT_GENERIC_MASK;
    REG(RGR1_SW_INIT_1) = v;
}

static void perst_set(int assert) {
    uint32_t v = REG(RGR1_SW_INIT_1);
    if (assert) v |= RGR1_PERST_MASK;
    else        v &= ~RGR1_PERST_MASK;
    REG(RGR1_SW_INIT_1) = v;
}

// Program outbound window 0: CPU [cpu, cpu+size) -> PCIe pcie_addr.
static void set_outbound_window(uint64_t cpu, uint64_t pcie, uint64_t size) {
    REG(MISC_CPU_2_PCIE_MEM_WIN0_LO) = (uint32_t)(pcie & 0xffffffff);
    REG(MISC_CPU_2_PCIE_MEM_WIN0_HI) = (uint32_t)(pcie >> 32);

    uint32_t base_mb  = (uint32_t)(cpu >> 20);
    uint32_t limit_mb = (uint32_t)((cpu + size - 1) >> 20);
    REG(MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT) =
        ((base_mb & 0xfff) << 20) | ((limit_mb & 0xfff) << 4);
    REG(MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI)  = base_mb  >> 12;
    REG(MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI) = limit_mb >> 12;
}

uintptr_t pcie_init(void) {
    uart_puts("[PCIE] bring-up...\n");

    // 1. Reset: assert bridge soft-reset and endpoint PERST#, then release the
    //    bridge reset and let the PHY settle.
    bridge_sw_init(1);
    perst_set(1);
    usleep(2000);
    bridge_sw_init(0);
    usleep(2000);

    uint32_t rev = REG(MISC_REVISION) & 0xffff;
    uart_hex("[PCIE] revision: ", rev);
    if (rev == 0 || rev == 0xffff) {
        uart_puts("[PCIE] no controller (expected on QEMU)\n");
        return 0;                                   // not real hardware
    }

    // 2. Controller config: enable SCB access, return UR on bad config reads,
    //    128-byte max burst (MISC_CTRL bits 12,13 set; burst field cleared).
    uint32_t mc = REG(MISC_MISC_CTRL);
    mc |= (1u << 12);     // SCB_ACCESS_EN
    mc |= (1u << 13);     // CFG_READ_UR_MODE
    mc &= ~(3u << 20);    // MAX_BURST_SIZE = 128
    REG(MISC_MISC_CTRL) = mc;

    // 3. Inbound window: let the endpoint DMA into the low 4 GiB of system RAM
    //    1:1 (RC_BAR2 base 0, size 4 GiB; encoded size 0x11 = log2(4G)-15).
    REG(MISC_RC_BAR2_CONFIG_LO) = 0x00000011;
    REG(MISC_RC_BAR2_CONFIG_HI) = 0;
    REG(MISC_RC_BAR1_CONFIG_LO) = 0;                // disable BAR1/BAR3
    REG(MISC_RC_BAR3_CONFIG_LO) = 0;

    // 4. Outbound window for the VL805's MMIO.
    set_outbound_window(OUTBOUND_CPU, OUTBOUND_PCIE, OUTBOUND_SIZE);

    // 5. Release PERST# and wait for the data link to come up.
    perst_set(0);
    int up = 0;
    for (int i = 0; i < 100; i++) {                 // up to ~0.5 s
        uint32_t st = REG(MISC_PCIE_STATUS);
        if ((st & STATUS_PHYLINKUP) && (st & STATUS_DL_ACTIVE)) { up = 1; break; }
        usleep(5000);
    }
    uart_hex("[PCIE] status: ", REG(MISC_PCIE_STATUS));
    if (!up) { uart_puts("[PCIE] link did not come up\n"); return 0; }
    uart_puts("[PCIE] link up\n");

    // 6. Map the outbound window into the CPU's address space (Device memory).
    mmu_map_device_block(OUTBOUND_CPU);

    // 7. Ask the VideoCore firmware to (re)load the VL805 firmware now that the
    //    device is visible on PCIe. dev_addr = (bus<<20)|(dev<<15)|(fn<<12).
    uint32_t dev_addr = (VL805_BUS << 20) | (VL805_DEV << 15) | (VL805_FN << 12);
    mbox[0] = 7 * 4; mbox[1] = 0;
    mbox[2] = 0x00030058;                            // NOTIFY_XHCI_RESET
    mbox[3] = 4; mbox[4] = 4;
    mbox[5] = dev_addr;
    mbox[6] = 0;
    if (mbox_call()) uart_puts("[PCIE] VL805 firmware notified\n");
    else             uart_puts("[PCIE] VL805 firmware notify failed (continuing)\n");
    usleep(200000);                                  // give firmware time to load

    // 8. Identify the VL805 in config space.
    uint32_t id = cfg_read(VL805_BUS, VL805_DEV, VL805_FN, 0x00);
    uart_hex("[PCIE] dev id: ", id);
    if (id == 0xffffffff || id == 0) {
        uart_puts("[PCIE] VL805 not responding\n");
        return 0;
    }

    // 9. Configure the bridge (bus 0): primary=0, secondary=1, subordinate=1,
    //    and enable memory space + bus mastering.
    cfg_write(0, 0, 0, 0x18, 0x00010100);            // lat | sub<<16 | sec<<8 | pri
    cfg_write(0, 0, 0, 0x04, cfg_read(0, 0, 0, 0x04) | 0x06);

    // 10. Assign the VL805 BAR0 (64-bit memory) and enable it.
    cfg_write(VL805_BUS, VL805_DEV, VL805_FN, 0x10, (uint32_t)OUTBOUND_PCIE);  // BAR0 lo
    cfg_write(VL805_BUS, VL805_DEV, VL805_FN, 0x14, 0);                         // BAR0 hi
    cfg_write(VL805_BUS, VL805_DEV, VL805_FN, 0x04,
              cfg_read(VL805_BUS, VL805_DEV, VL805_FN, 0x04) | 0x06);           // MEM | BM

    uart_hex("[PCIE] VL805 xHCI MMIO at CPU ", (uint32_t)(OUTBOUND_CPU >> 4));
    uart_puts("[PCIE] ok\n");
    return (uintptr_t)OUTBOUND_CPU;
}
