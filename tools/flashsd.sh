#!/usr/bin/env bash
#
# Interactively flash the BerryBasic SD-card image to a removable device.
#
# Presents a numbered list of candidate disks (size / bus / model), refuses to
# offer the disk your system is running from, makes you confirm by re-typing the
# device name, then wipes old partition tables and writes the image.
#
# This exists because picking the wrong /dev node is the single most common way
# to brick the wrong disk - or to flash a USB stick while the Pi boots a
# different SD card. Always match the SIZE shown here to your real card.
#
# Usage:  tools/flashsd.sh [image.img]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:-$ROOT/berrybasic-sd.img}"

[ -f "$IMG" ] || { echo "error: image '$IMG' not found - run 'make sdimage' first"; exit 1; }
command -v lsblk >/dev/null || { echo "error: lsblk not found"; exit 1; }

IMG_SIZE=$(stat -c %s "$IMG")
printf 'Image: %s (%s)\n\n' "$IMG" "$(numfmt --to=iec --suffix=B "$IMG_SIZE" 2>/dev/null || echo "${IMG_SIZE} bytes")"

# Which whole-disks does the running system live on?  Never offer them.  We
# resolve the *physical* disks backing /, /boot and /boot/efi via lsblk's
# inverse tree (-s), so this works even when root is on LVM/RAID/LUKS where the
# mount source (e.g. /dev/mapper/...) has no direct parent disk.
declare -A PROTECTED=()
for mp in / /boot /boot/efi; do
    src="$(findmnt -no SOURCE "$mp" 2>/dev/null || true)"
    [ -n "$src" ] || continue
    while read -r disk; do
        [ -n "$disk" ] && PROTECTED["$disk"]=1
    done < <(lsblk -snlo NAME,TYPE "$src" 2>/dev/null | awk '$2=="disk"{print $1}')
done

# Collect candidate whole disks (TYPE=disk).  Prefer removable / USB / MMC, but
# list everything except the system disk so an oddly-enumerated reader still
# shows up - clearly flagged.
mapfile -t DISKS < <(lsblk -dn -o NAME,TYPE | awk '$2=="disk"{print $1}')

idx=0
declare -a SEL_NAME SEL_DESC
echo "Devices you can flash to:"
echo
for d in "${DISKS[@]}"; do
    [ -n "${PROTECTED[$d]:-}" ] && continue         # never a disk that backs the running system
    size=$(lsblk -dn -o SIZE  "/dev/$d")
    tran=$(lsblk -dn -o TRAN  "/dev/$d" 2>/dev/null || echo "?")
    rm=$(lsblk -dn -o RM      "/dev/$d")
    model=$(lsblk -dn -o MODEL "/dev/$d" 2>/dev/null | sed 's/[[:space:]]*$//')
    mounts=$(lsblk -ln -o MOUNTPOINTS "/dev/$d" 2>/dev/null | tr '\n' ' ' | sed 's/[[:space:]]*$//')
    flag=""
    [ "$rm" = "1" ] && flag="removable"
    [ "$tran" = "usb" ] || [ "$tran" = "mmc" ] || [ "$tran" = "sd" ] || { [ -z "$flag" ] && flag="FIXED DISK - be careful"; }
    idx=$((idx+1))
    SEL_NAME[$idx]="$d"
    SEL_DESC[$idx]="/dev/$d  ${size}  [${tran:-?}]  ${model:-unknown}${flag:+  <$flag>}${mounts:+  mounted:$mounts}"
    printf '  %d) %s\n' "$idx" "${SEL_DESC[$idx]}"
done
echo

if [ "$idx" -eq 0 ]; then
    echo "No flashable devices found (the system disk is excluded)."
    echo "Insert your SD card and re-run."
    exit 1
fi

printf 'Select a device number to flash [1-%d], or q to quit: ' "$idx"
read -r choice
[ "$choice" = "q" ] && { echo "aborted."; exit 0; }
case "$choice" in
    ''|*[!0-9]*) echo "error: not a number"; exit 1;;
esac
[ "$choice" -ge 1 ] && [ "$choice" -le "$idx" ] || { echo "error: out of range"; exit 1; }

DEV="/dev/${SEL_NAME[$choice]}"
echo
echo "About to ERASE and flash:"
echo "    ${SEL_DESC[$choice]}"
echo
echo "ALL DATA ON $DEV WILL BE DESTROYED."
printf 'Type the device name exactly (%s) to confirm: ' "$DEV"
read -r confirm
[ "$confirm" = "$DEV" ] || { echo "names did not match - aborted."; exit 1; }

echo
echo ">> wiping old partition tables on $DEV ..."
sudo wipefs -a "$DEV"
sudo sgdisk --zap-all "$DEV" 2>/dev/null || true   # nuke GPT incl. backup at disk end

echo ">> writing image (this can take a while) ..."
sudo dd if="$IMG" of="$DEV" bs=4M conv=fsync status=progress
sudo sync

# Force the kernel to forget any cached copy and re-read straight from the card.
# A counterfeit / dead card accepts the write (so dd and an immediate read look
# fine) but does not persist it - on a real re-read the partition table reverts
# to garbage. This catches that instead of leaving you to debug a non-booting Pi.
echo
echo ">> verifying the write actually persisted on the card ..."
sudo blockdev --flushbufs "$DEV" 2>/dev/null || true
sudo partprobe "$DEV" 2>/dev/null || true
sleep 1
VERIFY="$(sudo fdisk -l "$DEV" 2>/dev/null || true)"
echo "$VERIFY"
echo
if echo "$VERIFY" | grep -Eq '(^|[^0-9])2048[[:space:]].*(FAT32|[^0-9]c[[:space:]])'; then
    echo "OK: the 0x0c FAT32 partition at sector 2048 is present after a fresh re-read."
    echo "Put the card in the Pi and boot."
else
    echo "############################################################"
    echo "# WARNING: the expected FAT32 partition at sector 2048 did  #"
    echo "# NOT survive a fresh re-read of the card.                  #"
    echo "#                                                          #"
    echo "# The image is fine; the CARD is almost certainly dead or   #"
    echo "# counterfeit - it accepts writes but does not store them.  #"
    echo "# Verify it with:  sudo f3probe --destructive /dev/<dev>    #"
    echo "# (a real card shows usable size ~= its labelled size).     #"
    echo "# Use a different, genuine SD card.                         #"
    echo "############################################################"
    exit 1
fi
