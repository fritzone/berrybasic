# Running BerryBasic on a real Raspberry Pi 4

BerryBasic is developed under QEMU (`make run`) but also runs on real Pi 4
hardware. This document explains how to build a bootable SD card and what works
/ doesn't on real silicon.

## Build a bootable SD-card image

```sh
make            # build build/kernel8.img
make sdimage    # build berrybasic-sd.img (downloads Pi 4 firmware on first run)
```

`make sdimage` produces `berrybasic-sd.img`: an MBR disk with one bootable FAT32
partition containing

| file          | what it is                                            |
|---------------|-------------------------------------------------------|
| `start4.elf`  | Pi 4 GPU firmware (fetched from raspberrypi/firmware) |
| `fixup4.dat`  | companion linker data for `start4.elf`                |
| `config.txt`  | boot configuration (`boot/config.txt`)                |
| `kernel8.img` | our bare-metal kernel                                 |
| `*.BAS`       | any programs in `examples/` (and your saved programs) |

The Pi 4 boot ROM + on-board EEPROM load `start4.elf`, which reads `config.txt`
and runs `kernel8.img`. No `bootcode.bin` is needed on the Pi 4 (it lives in the
SoC). The same FAT32 partition is what BASIC `SAVE`/`LOAD`/`CAT` use, so the card
is also your disk.

## Flash it

```sh
# find your card with lsblk; BE SURE of the device name!
sudo dd if=berrybasic-sd.img of=/dev/sdX bs=4M conv=fsync status=progress
```

Insert the card, connect a display (HDMI0, the port nearest USB-C) and power up.

## What works on real hardware

- **Display** – HDMI framebuffer (800x600 by default; override with
  `make CFLAGS_EXTRA="-DFB_WIDTH=1024 -DFB_HEIGHT=768"`). The framebuffer region
  is mapped non-cacheable (`mmu_set_noncached`) so CPU writes are coherent with
  the GPU scan-out.
- **SD card LOAD/SAVE** – the driver probes the Pi 4 EMMC2 controller
  (`0xFE340000`) first, falling back to the legacy controller QEMU uses. Mailbox
  buffers get cache maintenance (`dcache_clean_inval`) so GPU/CPU stay coherent.
- **Serial console** – `enable_uart=1` gives a 115200 8N1 console on GPIO14/15
  (pins 8/10). Useful for headless bring-up.

## USB keyboard

Two keyboard paths are tried at boot:

1. **USB-C port** via the **DWC2 OTG controller** (this is also what QEMU
   emulates). Tried first.
2. **USB-A ports** via the **VL805 xHCI controller on PCIe** — `src/pcie.c`
   brings up the BCM2711 PCIe root complex and `src/xhci.c` is a minimal xHCI
   driver that enumerates one HID boot keyboard. Tried if no USB-C keyboard is
   found.

So a keyboard in any USB-A port should work. If it doesn't, the **UART serial
console** always works as a fallback terminal.

> **Note on the USB-A path:** the PCIe + xHCI bring-up can only run on real
> hardware (QEMU's `raspi4b` has no PCIe), so it is **validated by the boot log,
> not in CI**. Every step prints a `[PCIE]` / `[XHCI]` line (link-up, slot id,
> device VID/PID, endpoint, "keyboard ready"); the last line printed tells you
> where it stopped. The PCIe outbound window is mapped at CPU `0x6_00000000`,
> which is why the MMU now uses a 39-bit VA (`mmu.c`, `T0SZ=25`).

## Reading the boot log without a serial cable

The kernel writes the entire boot log to **`BOOTLOG.TXT`** in the root of the SD
card's FAT partition on every boot. So to diagnose the USB-A keyboard (or
anything else) with no serial adapter:

1. Power on the Pi, wait a few seconds, power off.
2. Move the SD card to your PC and open **`BOOTLOG.TXT`**.

It contains everything that would have gone to the serial console — the `[BOOT]`,
`[SD]`, `[USB]`, `[PCIE]` and `[XHCI]` lines. The log is flushed:

- after the keyboard probe (so you always get boot + SD + USB even if USB-A
  detection just quietly fails), and
- again after the USB-A bring-up (the full log), and
- from the exception handler if the kernel crashes (the fault details land in the
  file too).

If `BOOTLOG.TXT` doesn't appear at all, the SD write path itself failed — which
is itself useful information (it means the EMMC2 driver, not USB, is the problem).

Paste the contents back and the failing stage can be pinpointed.

## Differences from QEMU (for the curious)

QEMU's `raspi4b` doesn't model the CPU caches and wires the SD card to the legacy
SDHCI controller, so a few things differ on real silicon. These are all handled:

| concern            | QEMU                | real Pi 4            | handled by                         |
|--------------------|---------------------|----------------------|------------------------------------|
| SD controller      | legacy `0xFE300000` | EMMC2 `0xFE340000`   | `sd_init` probes both              |
| SD base clock      | lenient             | real rate            | mailbox `query_base_clock`         |
| framebuffer cache  | coherent            | needs non-cacheable  | `mmu_set_noncached` on the FB      |
| mailbox cache      | coherent            | needs maintenance    | `dcache_clean_inval` in `mbox_call`|
| exception level    | EL2/EL1             | EL2                  | `boot.S` drops EL3/EL2 -> EL1      |
