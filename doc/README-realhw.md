# Running BerryBasic on a real Raspberry Pi 4

BerryBasic is developed under QEMU (`make run`) but also runs on real Pi 4
hardware. This document explains how to build a bootable SD card and what works
/ doesn't on real silicon.

## Build a bootable SD-card image

```sh
make            # build build/kernel8.img
make sdimage    # build berrybasic-sd.img (downloads Pi 4 firmware on first run)
make flash      # interactively flash that image to a removable card (safe picker)
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

The easy, safe way - an interactive helper that lists removable devices (size /
bus / model), **excludes the disk your system is running from**, makes you
re-type the device name to confirm, then wipes old partition tables and writes
the image:

```sh
make flash        # builds the image if needed, then runs tools/flashsd.sh
```

Pick the device whose **SIZE matches your real card** (a "32 GB" card shows as
~29.7G). This avoids the classic trap of flashing a USB stick while the Pi boots
a different SD card.

<details>
<summary>Manual way (if you prefer raw <code>dd</code>)</summary>

```sh
# find your card with lsblk; BE SURE of the device name!  (/dev/sda is often
# the system disk - confirm size/TRAN: lsblk -o NAME,SIZE,TRAN,MODEL /dev/sdX)
sudo wipefs -a /dev/sdX                        # erase any old MBR/GPT signatures
sudo sgdisk --zap-all /dev/sdX 2>/dev/null || true   # nuke leftover GPT (incl. backup)
sudo dd if=berrybasic-sd.img of=/dev/sdX bs=4M conv=fsync status=progress
sudo sync
```
</details>

The `wipefs`/`sgdisk` step matters on cards that previously held a GPT (most
pre-formatted / ex-Raspberry-Pi-OS cards): a 256 MiB image written with `dd`
does **not** erase the backup GPT header at the end of a large card, and that
leftover is enough to make the Pi boot ROM read the wrong (empty) filesystem
and print **"Firmware not found"**. See *Troubleshooting* below.

Verify before booting - this must show exactly one `0x0c` partition at sector
2048 and **no** "GPT" warning:

```sh
sudo fdisk -l /dev/sdX
```

Insert the card, connect a display (HDMI0, the port nearest USB-C) and power up.

### Reading the card on your PC (mounting)

The filesystem lives in a **partition**, not at the start of the disk, so you
cannot `mount -o loop berrybasic-sd.img` directly - that hits the MBR and fails
with *"wrong fs type, bad superblock"*. Use one of:

```sh
# the .img file: mount the partition by its 1 MiB offset (2048 * 512)
sudo mount -o loop,offset=1048576 berrybasic-sd.img /mnt/raspimg

# a flashed card: mount the partition device (note the trailing 1), not the disk
sudo mount /dev/sdX1 /mnt/raspimg
```

Or skip mounting entirely and use mtools against the image:

```sh
MTOOLS_SKIP_CHECK=1 mdir  -i berrybasic-sd.img@@1048576 ::
MTOOLS_SKIP_CHECK=1 mcopy -i berrybasic-sd.img@@1048576 ::BOOTLOG.TXT .
```

> After `dd`, if the kernel can't re-read the partition table (`partprobe` says
> *"device is in use"* / `BLKRRPART: busy`), an old partition of the card is
> still mounted - `sudo umount /dev/sdX*` then re-insert the card.

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
2. **USB-A ports** via the **VL805 xHCI controller on PCIe** — `drivers/pcie.c`
   brings up the BCM2711 PCIe root complex and `drivers/xhci.c` is a minimal xHCI
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

## Troubleshooting boot

The Pi 4 prints two very different kinds of log to the HDMI screen, and it helps
to know which one you're looking at:

1. **The GPU bootloader / EEPROM** (Pi firmware) runs *first*. It reads the SD
   card, loads `start4.elf`, then `config.txt`, then `kernel8.img`. Its log
   looks like `SD: card detected ...`, `part: 0 mbr [...]`, `fw: start.elf
   fixup.dat`, `Boot mode: ...`, `USB2 root HUB ...`. **This is not our code.**
2. **Our kernel** (`kernel8.img`) runs only after the firmware hands off, and
   only *then* do `BOOTLOG.TXT` and the `[BOOT]`/`[SD]`/`[PCIE]`/`[XHCI]` lines
   appear.

So if you see firmware log but **no `BOOTLOG.TXT` is written**, the firmware
never reached our kernel - it's a *firmware-loading* problem, not a kernel bug.
Common cases:

| symptom in the firmware log | cause | fix |
|---|---|---|
| `part: 0 mbr [0xee:...]` | sector 0 is a **GPT protective MBR** - leftover GPT from the card's old format | `wipefs -a` + `sgdisk --zap-all`, then re-flash (see *Flash it*) |
| `root dir ... entries 0` + a huge `c-count` | firmware is reading the **wrong/empty** filesystem (the old big partition), not our 255 MiB one | same as above |
| `Firmware not found` | `start4.elf`/`fixup4.dat` not found in the partition the firmware read | same as above; verify `fdisk -l` shows one `0x0c` part at 2048 |
| `HUB ... init` / `xHC-CMD err: ... type: 11` after "Firmware not found" | the **firmware's** own fallback "boot from USB mass storage" attempt - *not* our xHCI driver | irrelevant; fix the SD boot above |

The fastest way to confirm the card is bootable: with it in your PC,
`sudo fdisk -l /dev/sdX` must show exactly one bootable `W95 FAT32 (LBA)`
partition at sector 2048 and no GPT warning, and `start4.elf` + `config.txt` +
`kernel8.img` must be visible in that partition.

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
