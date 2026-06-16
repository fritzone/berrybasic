#!/usr/bin/env bash
#
# Build a bootable Raspberry Pi 4 SD-card image for BerryBasic.
#
# The image has TWO MBR FAT32 partitions:
#   1) boot/system (bootable) - start4.elf, fixup4.dat, config.txt, kernel8.img,
#      bcm2711-rpi-4-b.dtb. This is what the Pi firmware boots from; BASIC never
#      touches it.
#   2) BASIC programs - the user's .BAS files. SAVE/LOAD/CAT operate only here,
#      so listing files never shows the system files, and you can drop programs
#      onto this partition from a PC without risking the boot files.
#
# No root/loopback needed: we format each partition-sized file with mkfs.fat,
# fill it with mtools, then assemble the final image with an MBR via sfdisk.
#
# Usage:  tools/mksdimage.sh [output.img] [size_MB]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/berrybasic-sd.img}"
SIZE_MB="${2:-256}"
FW_DIR="$ROOT/firmware"
KERNEL="$ROOT/build/kernel8.img"
CONFIG="$ROOT/boot/config.txt"

# Layout (sectors of 512 B; 2048 sectors = 1 MiB):
#   gap   : LBA 0..2047            (MBR + alignment)
#   boot  : LBA 2048, 64 MiB       (firmware + kernel)
#   data  : right after, fills the rest (BASIC programs)
BOOT_START=2048
BOOT_MB=64
BOOT_SECTORS=$(( BOOT_MB * 2048 ))
DATA_START=$(( BOOT_START + BOOT_SECTORS ))
DATA_SECTORS=$(( SIZE_MB * 2048 - DATA_START ))
DATA_MB=$(( DATA_SECTORS / 2048 ))
FW_BASE="https://github.com/raspberrypi/firmware/raw/master/boot"

[ -f "$KERNEL" ] || { echo "error: $KERNEL not found - run 'make' first"; exit 1; }
[ -f "$CONFIG" ] || { echo "error: $CONFIG not found"; exit 1; }

# --- Fetch the Pi 4 GPU firmware + device tree (cached in firmware/) ---------
# The device tree (bcm2711-rpi-4-b.dtb) matters: on Pi 4 the firmware's 64-bit
# kernel handoff expects a DTB to be present. Without it the firmware loads the
# GPU firmware (shows the splash) but never starts kernel8.img. Our bare-metal
# kernel ignores the DTB pointer passed in x0.
mkdir -p "$FW_DIR"
for f in start4.elf fixup4.dat bcm2711-rpi-4-b.dtb; do
    if [ ! -f "$FW_DIR/$f" ]; then
        echo "downloading $f ..."
        curl -fsSL "$FW_BASE/$f" -o "$FW_DIR/$f"
    fi
done

# --- Build the boot (system) partition --------------------------------------
BOOTPART="$(mktemp)"
DATAPART="$(mktemp)"
trap 'rm -f "$BOOTPART" "$DATAPART"' EXIT
export MTOOLS_SKIP_CHECK=1

dd if=/dev/zero of="$BOOTPART" bs=1M count="$BOOT_MB" status=none
mkfs.fat -F 32 -n BERRYBOOT "$BOOTPART" >/dev/null
mcopy -i "$BOOTPART" "$FW_DIR/start4.elf"            ::start4.elf
mcopy -i "$BOOTPART" "$FW_DIR/fixup4.dat"            ::fixup4.dat
mcopy -i "$BOOTPART" "$FW_DIR/bcm2711-rpi-4-b.dtb"   ::bcm2711-rpi-4-b.dtb
mcopy -i "$BOOTPART" "$CONFIG"                       ::config.txt
mcopy -i "$BOOTPART" "$KERNEL"                       ::kernel8.img

# --- Build the data (BASIC programs) partition ------------------------------
dd if=/dev/zero of="$DATAPART" bs=1M count="$DATA_MB" status=none
mkfs.fat -F 32 -n BERRYDATA "$DATAPART" >/dev/null
# Seed the user's example programs here (this is what CAT will list).
for bas in "$ROOT"/examples/*.bas; do
    [ -e "$bas" ] || break
    mcopy -i "$DATAPART" "$bas" "::$(basename "$bas" | tr '[:lower:]' '[:upper:]')"
done

# --- Assemble the final image with a 2-partition MBR ------------------------
dd if=/dev/zero of="$OUT" bs=1M count="$SIZE_MB" status=none
# #1 bootable FAT32-LBA system partition, #2 FAT32-LBA data partition.
sfdisk --quiet "$OUT" <<EOF
${BOOT_START},${BOOT_SECTORS},0c,*
${DATA_START},${DATA_SECTORS},0c
EOF
dd if="$BOOTPART" of="$OUT" bs=512 seek="$BOOT_START" conv=notrunc status=none
dd if="$DATAPART" of="$OUT" bs=512 seek="$DATA_START" conv=notrunc status=none

echo
echo "Created $OUT (${SIZE_MB} MiB): boot partition ${BOOT_MB} MiB + data partition ${DATA_MB} MiB."
echo
echo "Flash it the safe way (lists devices, excludes your system disk, confirms):"
echo "  make flash          # or: tools/flashsd.sh $OUT"
echo
echo "Manual alternative (replace /dev/sdX with your SD card - check with lsblk,"
echo "and match its SIZE to your real card!):"
echo "  sudo wipefs -a /dev/sdX                 # erase any old MBR/GPT first"
echo "  sudo sgdisk --zap-all /dev/sdX || true  # nuke leftover GPT (incl. backup at card end)"
echo "  sudo dd if=$OUT of=/dev/sdX bs=4M conv=fsync status=progress"
echo "  sudo sync"
echo
echo "Then verify - 'sudo fdisk -l /dev/sdX' must show a bootable 0x0c FAT32 partition"
echo "at sector 2048 (the system) plus a second 0x0c FAT32 data partition, total size"
echo "matching your card, and NO 'GPT detected' warning. A leftover GPT (type 0xee in"
echo "the Pi bootloader log) or a too-small total size makes the Pi read the wrong"
echo "filesystem and print 'Firmware not found'."

