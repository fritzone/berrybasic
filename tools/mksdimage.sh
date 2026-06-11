#!/usr/bin/env bash
#
# Build a bootable Raspberry Pi 4 SD-card image for BerryBasic.
#
# The image has a single MBR FAT32 partition containing:
#   - start4.elf, fixup4.dat   (Pi 4 GPU firmware, fetched from raspberrypi/firmware)
#   - config.txt               (boot/config.txt)
#   - kernel8.img              (our bare-metal kernel, from build/)
# The same partition is where BASIC LOAD/SAVE reads/writes .BAS files, so the
# disk doubles as the user's storage.
#
# No root/loopback needed: we format a partition-sized file with mkfs.fat, fill
# it with mtools, then assemble the final image with an MBR via sfdisk.
#
# Usage:  tools/mksdimage.sh [output.img] [size_MB]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/berrybasic-sd.img}"
SIZE_MB="${2:-256}"
FW_DIR="$ROOT/firmware"
KERNEL="$ROOT/build/kernel8.img"
CONFIG="$ROOT/boot/config.txt"

PART_START=2048                       # first partition LBA (1 MiB alignment)
FW_BASE="https://github.com/raspberrypi/firmware/raw/master/boot"

[ -f "$KERNEL" ] || { echo "error: $KERNEL not found - run 'make' first"; exit 1; }
[ -f "$CONFIG" ] || { echo "error: $CONFIG not found"; exit 1; }

# --- Fetch the Pi 4 GPU firmware (cached in firmware/) ----------------------
mkdir -p "$FW_DIR"
for f in start4.elf fixup4.dat; do
    if [ ! -f "$FW_DIR/$f" ]; then
        echo "downloading $f ..."
        curl -fsSL "$FW_BASE/$f" -o "$FW_DIR/$f"
    fi
done

# --- Build the FAT32 partition image ----------------------------------------
PART_MB=$(( SIZE_MB - 1 ))            # leave 1 MiB for the MBR/alignment gap
PART="$(mktemp)"
trap 'rm -f "$PART"' EXIT
dd if=/dev/zero of="$PART" bs=1M count="$PART_MB" status=none
mkfs.fat -F 32 -n BERRYBOOT "$PART" >/dev/null

export MTOOLS_SKIP_CHECK=1
mcopy -i "$PART" "$FW_DIR/start4.elf"  ::start4.elf
mcopy -i "$PART" "$FW_DIR/fixup4.dat"  ::fixup4.dat
mcopy -i "$PART" "$CONFIG"             ::config.txt
mcopy -i "$PART" "$KERNEL"             ::kernel8.img
# Optionally seed some example programs.
for bas in "$ROOT"/examples/*.bas; do
    [ -e "$bas" ] || break
    mcopy -i "$PART" "$bas" "::$(basename "$bas" | tr '[:lower:]' '[:upper:]')"
done

# --- Assemble the final image with an MBR -----------------------------------
dd if=/dev/zero of="$OUT" bs=1M count="$SIZE_MB" status=none
# Single bootable FAT32-LBA (type 0x0c) partition starting at PART_START.
echo "${PART_START},,0c,*" | sfdisk --quiet "$OUT"
dd if="$PART" of="$OUT" bs=512 seek="$PART_START" conv=notrunc status=none

echo
echo "Created $OUT (${SIZE_MB} MiB)."
echo "Flash it with:  sudo dd if=$OUT of=/dev/sdX bs=4M conv=fsync status=progress"
echo "(replace /dev/sdX with your SD card device)."
