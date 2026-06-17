#include "sd.h"
#include "uart.h"
#include "mailbox.h"

// ---------------------------------------------------------------------------
// BCM2711 SD host controller (SDHCI) driver, PIO, 512-byte blocks.
//
// There are TWO SDHCI controllers on a Pi 4. The microSD slot is on the EMMC2
// (Arasan) at 0xFE340000 on REAL hardware, but QEMU's raspi4b wires the card to
// the LEGACY controller at 0xFE300000 instead. So sd_init() probes both and
// uses whichever actually has a card. The register layout is the same.
// ---------------------------------------------------------------------------
#define EMMC2_BASE       0xFE340000UL    // real Pi 4 microSD slot
#define EMMC_LEGACY_BASE 0xFE300000UL    // QEMU raspi4b

static uintptr_t emmc_base = EMMC2_BASE;  // active controller (chosen at init)
#define R(off)           (*(volatile uint32_t *)(emmc_base + (off)))

#define EMMC_ARG2        0x00
#define EMMC_BLKSIZECNT  0x04
#define EMMC_ARG1        0x08
#define EMMC_CMDTM       0x0C
#define EMMC_RESP0       0x10
#define EMMC_RESP1       0x14
#define EMMC_RESP2       0x18
#define EMMC_RESP3       0x1C
#define EMMC_DATA        0x20
#define EMMC_STATUS      0x24
#define EMMC_CONTROL0    0x28
#define EMMC_CONTROL1    0x2C
#define EMMC_INTERRUPT   0x30
#define EMMC_IRPT_MASK   0x34
#define EMMC_IRPT_EN     0x38
#define EMMC_CONTROL2    0x3C
#define EMMC_SLOTISR_VER 0xFC

// STATUS (Present State) register bits
#define SR_CMD_INHIBIT   0x00000001
#define SR_DAT_INHIBIT   0x00000002
#define SR_READ_AVAIL    0x00000800
#define SR_WRITE_AVAIL   0x00000400
#define SR_CARD_INSERTED 0x00010000

// CONTROL1 bits
#define C1_CLK_INTLEN    0x00000001
#define C1_CLK_STABLE    0x00000002
#define C1_CLK_EN        0x00000004
#define C1_SRST_HC       0x01000000
#define C1_SRST_CMD      0x02000000
#define C1_SRST_DATA     0x04000000

// INTERRUPT bits
#define INT_CMD_DONE     0x00000001
#define INT_DATA_DONE    0x00000002
#define INT_READ_RDY     0x00000020
#define INT_WRITE_RDY    0x00000010
#define INT_ERROR_MASK   0xFFFF0000

// CMDTM: command index + response/transfer flags (occupies the upper 16 bits of
// the 0x0C register; the lower 16 bits are the transfer mode).
#define CMD_RSPNS_NONE   0x00000000
#define CMD_RSPNS_136    0x00010000
#define CMD_RSPNS_48     0x00020000
#define CMD_RSPNS_48B    0x00030000   // 48-bit with busy
#define CMD_CRCCHK_EN    0x00080000
#define CMD_IXCHK_EN     0x00100000
#define CMD_ISDATA       0x00200000
#define TM_DAT_CARD2HOST 0x00000010   // read
#define TM_BLKCNT_EN     0x00000002
#define TM_MULTI_BLOCK   0x00000020
#define CMD_INDEX(n)     ((uint32_t)(n) << 24)

// SD commands we use (index | response/data flags).
#define CMD_GO_IDLE      (CMD_INDEX(0)  | CMD_RSPNS_NONE)
#define CMD_SEND_IF_COND (CMD_INDEX(8)  | CMD_RSPNS_48  | CMD_CRCCHK_EN)
#define CMD_APP_CMD      (CMD_INDEX(55) | CMD_RSPNS_48  | CMD_CRCCHK_EN)
#define CMD_APP_CMD_RCA  (CMD_INDEX(55) | CMD_RSPNS_48  | CMD_CRCCHK_EN)
#define ACMD_SD_SEND_OP  (CMD_INDEX(41) | CMD_RSPNS_48)
#define CMD_ALL_SEND_CID (CMD_INDEX(2)  | CMD_RSPNS_136 | CMD_CRCCHK_EN)
#define CMD_SEND_REL_ADDR (CMD_INDEX(3) | CMD_RSPNS_48  | CMD_CRCCHK_EN)
#define CMD_SELECT_CARD  (CMD_INDEX(7)  | CMD_RSPNS_48B | CMD_CRCCHK_EN)
#define CMD_SET_BLOCKLEN (CMD_INDEX(16) | CMD_RSPNS_48  | CMD_CRCCHK_EN)
#define CMD_READ_SINGLE  (CMD_INDEX(17) | CMD_RSPNS_48  | CMD_CRCCHK_EN | CMD_ISDATA | TM_DAT_CARD2HOST)
#define CMD_WRITE_SINGLE (CMD_INDEX(24) | CMD_RSPNS_48  | CMD_CRCCHK_EN | CMD_ISDATA)

#define TIMER_CLO (*(volatile uint32_t *)(0xFE003004UL))

static uint32_t sd_rca;       // card relative address (<<16 for command args)
static int      sd_is_sdhc;   // 1 = high-capacity (block-addressed)

static void usleep(uint32_t us) {
    uint32_t t = TIMER_CLO;
    while (TIMER_CLO - t < us) { }
}

// Wait until any of `mask` bits set in STATUS clear (CMD/DAT inhibit). Returns 0
// on success, -1 on timeout.
static int wait_status_clear(uint32_t mask) {
    uint32_t t = TIMER_CLO;
    while (R(EMMC_STATUS) & mask) {
        if (TIMER_CLO - t > 500000) return -1;   // 0.5s
    }
    return 0;
}

static uint32_t sd_last_int;     // last raw INTERRUPT value (diagnostics)

// Wait until any of `mask` interrupt bits is raised; returns the INTERRUPT value
// (cleared), or 0 on timeout / error.
static uint32_t wait_interrupt(uint32_t mask) {
    uint32_t t = TIMER_CLO;
    uint32_t want = mask | INT_ERROR_MASK;
    while (!(R(EMMC_INTERRUPT) & want)) {
        if (TIMER_CLO - t > 300000) { sd_last_int = 0xDEAD0000; return 0; }   // 0.3s
    }
    uint32_t ints = R(EMMC_INTERRUPT);
    // Ack ONLY the bits we waited on (+ error bits). The controller may raise
    // several flags at once (e.g. CMD_DONE together with READ_RDY); clearing all
    // of them here would lose a flag a later wait is about to look for.
    R(EMMC_INTERRUPT) = ints & want;
    sd_last_int = ints;
    if (ints & INT_ERROR_MASK) return 0;
    return ints;
}

// Reset just the command-line circuit (SDHCI requires this after a command
// error before any further command will be accepted).
static void reset_cmd_line(void) {
    R(EMMC_CONTROL1) |= C1_SRST_CMD;
    uint32_t t = TIMER_CLO;
    while (R(EMMC_CONTROL1) & C1_SRST_CMD) {
        if (TIMER_CLO - t > 100000) break;
    }
}

// Issue a command with argument; returns 0 on success, -1 on failure.
static int sd_command(uint32_t cmd, uint32_t arg) {
    if (wait_status_clear(SR_CMD_INHIBIT)) return -1;
    R(EMMC_INTERRUPT) = R(EMMC_INTERRUPT);       // clear stale flags
    R(EMMC_ARG1) = arg;
    R(EMMC_CMDTM) = cmd;
    if (!wait_interrupt(INT_CMD_DONE)) {
        reset_cmd_line();                        // recover the CMD line after error
        return -1;
    }
    return 0;
}

static uint32_t sd_base_clock = 41666666u;       // controller base clock (from mailbox)

// Query the actual EMMC base clock rate from the GPU (mailbox tag 0x00030002,
// clock id 1 = EMMC). Falls back to a typical value if unavailable.
static uint32_t query_base_clock(void) {
    mbox[0] = 8 * 4; mbox[1] = 0;
    mbox[2] = 0x00030002; mbox[3] = 8; mbox[4] = 8;
    mbox[5] = 1;                                  // clock id: EMMC
    mbox[6] = 0;
    mbox[7] = 0;
    if (mbox_call() && mbox[6]) return mbox[6];
    return 41666666u;
}

// Set the SD clock to approximately `freq` Hz using the real base clock.
static int sd_set_clock(uint32_t freq) {
    // Wait for cmd/dat lines idle.
    if (wait_status_clear(SR_CMD_INHIBIT | SR_DAT_INHIBIT)) return -1;

    uint32_t base = sd_base_clock;
    uint32_t div = 1;
    while ((base / (div << 1)) > freq && div < 0x3FF) div++;

    uint32_t c1 = R(EMMC_CONTROL1);
    c1 &= ~C1_CLK_EN;                            // disable SD clock while changing
    R(EMMC_CONTROL1) = c1;
    usleep(10);

    c1 &= ~0xFFE0;                               // clear freq-select bits (8..15 + upper 6..7)
    c1 |= (div & 0xFF) << 8;
    c1 |= ((div >> 8) & 0x3) << 6;
    c1 |= C1_CLK_INTLEN;
    R(EMMC_CONTROL1) = c1;

    uint32_t t = TIMER_CLO;
    while (!(R(EMMC_CONTROL1) & C1_CLK_STABLE)) {
        if (TIMER_CLO - t > 500000) return -1;
    }
    R(EMMC_CONTROL1) |= C1_CLK_EN;
    usleep(10);
    return 0;
}

// Initialise the controller currently selected by `emmc_base`. With
// `need_present` set, bail quickly if no card is detected (so probing an empty
// controller costs no command timeouts). Returns 0 on success.
static int sd_init_on(int need_present) {
    sd_rca = 0;
    sd_is_sdhc = 0;

    // Reset the whole host controller.
    R(EMMC_CONTROL0) = 0;
    R(EMMC_CONTROL1) |= C1_SRST_HC;
    uint32_t t = TIMER_CLO;
    while (R(EMMC_CONTROL1) & C1_SRST_HC) {
        if (TIMER_CLO - t > 500000) return -1;
    }

    // Internal clock on, slow clock (~400 kHz) for identification.
    if (sd_set_clock(400000)) return -1;

    // SD bus power on, 3.3V (standard SDHCI power-control byte at offset 0x29).
    R(EMMC_CONTROL0) = 0x00000F00;
    usleep(1000);

    // Enable all interrupt status flags (we poll INTERRUPT, masking signalling).
    R(EMMC_IRPT_EN)   = 0xFFFFFFFF;
    R(EMMC_IRPT_MASK) = 0xFFFFFFFF;

    if (need_present && !(R(EMMC_STATUS) & SR_CARD_INSERTED)) return -1;   // no card here

    // CMD0: go idle.
    if (sd_command(CMD_GO_IDLE, 0)) return -1;

    // CMD8: check voltage range (0x1AA). Required for SD v2; we proceed regardless
    // (a timeout here just means an older card; the CMD line is auto-reset).
    sd_command(CMD_SEND_IF_COND, 0x000001AA);

    // ACMD41 loop: send host capacity support (HCS) until the card powers up.
    t = TIMER_CLO;
    for (;;) {
        if (sd_command(CMD_APP_CMD, 0)) return -1;
        if (sd_command(ACMD_SD_SEND_OP, 0x51FF8000)) return -1;
        uint32_t r = R(EMMC_RESP0);
        if (r & 0x80000000) {                    // card powered up
            sd_is_sdhc = (r & 0x40000000) ? 1 : 0;
            break;
        }
        if (TIMER_CLO - t > 1000000) return -1;
        usleep(10000);
    }

    // CMD2: get CID. CMD3: get relative card address.
    if (sd_command(CMD_ALL_SEND_CID, 0)) return -1;
    if (sd_command(CMD_SEND_REL_ADDR, 0)) return -1;
    sd_rca = R(EMMC_RESP0) & 0xFFFF0000;

    // Faster clock (~25 MHz) for data transfer.
    if (sd_set_clock(25000000)) return -1;

    // CMD7: select the card. CMD16: set 512-byte block length.
    if (sd_command(CMD_SELECT_CARD, sd_rca)) return -1;
    if (sd_command(CMD_SET_BLOCKLEN, 512)) return -1;
    return 0;
}

int sd_init(void) {
    sd_base_clock = query_base_clock();
    static const uintptr_t bases[2] = { EMMC2_BASE, EMMC_LEGACY_BASE };

    // Pass 1: prefer a controller that reports a card inserted (fast).
    for (int i = 0; i < 2; i++) {
        emmc_base = bases[i];
        if (sd_init_on(1) == 0) goto ready;
    }
    // Pass 2: card-detect may be unwired on some boards - try anyway.
    for (int i = 0; i < 2; i++) {
        emmc_base = bases[i];
        if (sd_init_on(0) == 0) goto ready;
    }
    uart_puts("[SD] no card found on either controller\n");
    return -1;

ready:
    uart_puts(emmc_base == EMMC2_BASE ? "[SD] EMMC2 controller, " : "[SD] legacy controller, ");
    uart_puts(sd_is_sdhc ? "card ready (SDHC)\n" : "card ready (SD)\n");
    return 0;
}

// Translate an LBA to the command argument: SDHC is block-addressed, standard SD
// is byte-addressed.
static uint32_t lba_arg(uint32_t lba) { return sd_is_sdhc ? lba : lba * 512; }

int sd_read(uint32_t lba, uint32_t count, void *buf) {
    uint32_t *p = (uint32_t *)buf;
    for (uint32_t b = 0; b < count; b++) {
        if (wait_status_clear(SR_DAT_INHIBIT)) return -1;
        R(EMMC_BLKSIZECNT) = (1 << 16) | 512;            // 1 block of 512 bytes
        if (sd_command(CMD_READ_SINGLE, lba_arg(lba + b))) return -1;
        if (!wait_interrupt(INT_READ_RDY)) return -1;
        for (int i = 0; i < 128; i++) *p++ = R(EMMC_DATA);   // 128 words = 512 bytes
        if (!wait_interrupt(INT_DATA_DONE)) return -1;
    }
    return 0;
}

int sd_write(uint32_t lba, uint32_t count, const void *buf) {
    const uint32_t *p = (const uint32_t *)buf;
    for (uint32_t b = 0; b < count; b++) {
        if (wait_status_clear(SR_DAT_INHIBIT)) return -1;
        R(EMMC_BLKSIZECNT) = (1 << 16) | 512;
        if (sd_command(CMD_WRITE_SINGLE, lba_arg(lba + b))) return -1;
        if (!wait_interrupt(INT_WRITE_RDY)) return -1;
        for (int i = 0; i < 128; i++) R(EMMC_DATA) = *p++;
        if (!wait_interrupt(INT_DATA_DONE)) return -1;
    }
    return 0;
}
