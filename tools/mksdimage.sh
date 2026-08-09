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
# Example programs live under /EXAMPLES, grouped by what they show (graphics,
# files, hardware, language, modules, seeds, pods). Each group directory is
# self-contained: an example that loads an asset carries its own copy, so you
# can CD into a group and run anything in it. This keeps the card root tidy.
if compgen -G "$ROOT/examples/*/" >/dev/null; then
    mmd -i "$DATAPART" ::EXAMPLES
    for dir in "$ROOT"/examples/*/; do
        [ -d "$dir" ] || continue
        group="$(basename "$dir" | tr '[:lower:]' '[:upper:]')"
        [ "$group" = "SEEDS" ] && continue   # has per-seed subdirs; shipped recursively below
        mmd -i "$DATAPART" "::EXAMPLES/$group"
        for file in "$dir"*; do
            [ -f "$file" ] || continue
            mcopy -i "$DATAPART" "$file" "::EXAMPLES/$group/$(basename "$file" | tr '[:lower:]' '[:upper:]')"
        done
    done
    # The graphical Example Browser sits at the top of /EXAMPLES (with its logo).
    # It CHAINs into each group to run an example and gets itself back. Load it
    # with:  CD EXAMPLES : LOAD "MENU" : RUN
    [ -e "$ROOT/examples/menu.bas" ] && \
        mcopy -o -i "$DATAPART" "$ROOT/examples/menu.bas" "::EXAMPLES/MENU.BAS"
    [ -e "$ROOT/examples/more.bas" ] && \
        mcopy -o -i "$DATAPART" "$ROOT/examples/more.bas" "::EXAMPLES/MORE.BAS"
    [ -e "$ROOT/examples/logo.png" ] && \
        mcopy -o -i "$DATAPART" "$ROOT/examples/logo.png" "::EXAMPLES/LOGO.PNG"
    # poddemo drives HELLO.POD / HYPOT.POD (built by 'make pods'); ship them
    # alongside it so the example is self-contained.
    if [ -d "$ROOT/examples/pods" ]; then
        for p in hello hypot; do
            [ -e "$ROOT/build/pods/$p.POD" ] && \
                mcopy -i "$DATAPART" "$ROOT/build/pods/$p.POD" "::EXAMPLES/PODS/$(echo $p | tr '[:lower:]' '[:upper:]').POD"
        done
    fi
fi
# Native seeds (built by 'make seeds') go in their own /seed directory: SEED/CALL
# resolve a bare "NAME.SED" to /seed/NAME.SED, and keeping them out of the root
# leaves CAT's default listing uncluttered with user programs.
if compgen -G "$ROOT/build/seeds/*.sed" >/dev/null; then
    mmd -i "$DATAPART" ::SEED
    for sed in "$ROOT"/build/seeds/*.sed; do
        [ -e "$sed" ] || break
        mcopy -i "$DATAPART" "$sed" "::SEED/$(basename "$sed" | tr '[:lower:]' '[:upper:]')"
    done
fi
# The seed examples: each in its own directory under /EXAMPLES/SEEDS holding the
# source, a GROW and a README, with a collecting GROW at the top. So on the
# machine you can `cd EXAMPLES/SEEDS`, `grow` to build them all (grow links the
# berry-libc from SEEDCORE.PKT), or `grow NAME` for one. See examples/seeds/.
if [ -d "$ROOT/examples/seeds" ]; then
    mmd -i "$DATAPART" ::EXAMPLES ::EXAMPLES/SEEDS 2>/dev/null || true
    for f in "$ROOT"/examples/seeds/*; do          # the collecting GROW, any .bas
        [ -f "$f" ] || continue
        mcopy -o -i "$DATAPART" "$f" "::EXAMPLES/SEEDS/$(basename "$f" | tr '[:lower:]' '[:upper:]')"
    done
    for d in "$ROOT"/examples/seeds/*/; do          # each seed's own directory
        [ -d "$d" ] || continue
        sub="$(basename "$d" | tr '[:lower:]' '[:upper:]')"
        mmd -i "$DATAPART" "::EXAMPLES/SEEDS/$sub" 2>/dev/null || true
        for f in "$d"*; do
            [ -f "$f" ] || continue
            mcopy -o -i "$DATAPART" "$f" "::EXAMPLES/SEEDS/$sub/$(basename "$f" | tr '[:lower:]' '[:upper:]')"
        done
    done
fi
# The precompiled example PODs (build/pods/*.POD - HELLO/HYPOT, the tcc demos)
# are NOT shipped to the root: they clutter it, and poddemo already carries its
# own copies next to it in /EXAMPLES/PODS. Keep the root to /SYS, /SEED,
# /EXAMPLES and the runtime BOOTLOG.TXT.

# System command PODs go in /sys. A bare word typed at the prompt (or RUN word)
# whose name matches /sys/WORD.POD runs it with the rest of the line as its
# arguments: this is how the machine becomes command-driven ('echo', 'sum',
# 'tcc', ...). tcc.POD is the self-hosted C compiler.
if compgen -G "$ROOT/build/sys/*.POD" >/dev/null; then
    mmd -i "$DATAPART" ::SYS || true
    for pod in "$ROOT"/build/sys/*.POD; do
        [ -e "$pod" ] || break
        mcopy -i "$DATAPART" "$pod" "::SYS/$(basename "$pod" | tr '[:lower:]' '[:upper:]')"
    done
fi

# Headers for the on-machine compiler: /sys/include is tcc's default include
# path, so `tcc -pod prog.c` finds <pod.h> (and the pod-libc headers) with no
# flags. This is what makes the machine self-hosting.
mmd -i "$DATAPART" ::SYS 2>/dev/null || true
mmd -i "$DATAPART" ::SYS/INCLUDE 2>/dev/null || true
mmd -i "$DATAPART" ::SYS/INCLUDE/SYS 2>/dev/null || true
# tcc's own freestanding headers (stddef.h, stdarg.h, float.h, pod.h, ...): the
# pod-libc headers pull these in for size_t/va_list, so the on-machine compiler
# needs them present or every compile fails at "#include <stddef.h>".
# -o: overwrite. berry_services.h and seed.h appear in BOTH the compiler include
# tree (as generated mirrors) and podlib/include (their real home), so the same
# uppercased name is written twice; without -o the second mcopy would fail.
for h in "$ROOT"/third_party/tinycc/include/*.h; do
    [ -e "$h" ] || break
    mcopy -o -i "$DATAPART" "$h" "::SYS/INCLUDE/$(basename "$h" | tr '[:lower:]' '[:upper:]')"
done
for h in "$ROOT"/podlib/include/*.h; do
    [ -e "$h" ] || break
    mcopy -o -i "$DATAPART" "$h" "::SYS/INCLUDE/$(basename "$h" | tr '[:lower:]' '[:upper:]')"
done
for h in "$ROOT"/podlib/include/sys/*.h; do
    [ -e "$h" ] || break
    mcopy -o -i "$DATAPART" "$h" "::SYS/INCLUDE/SYS/$(basename "$h" | tr '[:lower:]' '[:upper:]')"
done

# Seed packets (.PKT static libraries) go in /SYS/LIB, tcc's default library
# path, so `tcc -pod prog.c -l MATH` finds MATH.PKT with no flags.
if compgen -G "$ROOT/build/lib/*.PKT" >/dev/null; then
    mmd -i "$DATAPART" ::SYS/LIB 2>/dev/null || true
    for pkt in "$ROOT"/build/lib/*.PKT; do
        [ -e "$pkt" ] || break
        mcopy -i "$DATAPART" "$pkt" "::SYS/LIB/$(basename "$pkt" | tr '[:lower:]' '[:upper:]')"
    done
    # the entry object for main()-based (pod-libc) programs, linked explicitly
    [ -e "$ROOT/build/lib/crt0.o" ] && mcopy -i "$DATAPART" "$ROOT/build/lib/crt0.o" ::SYS/LIB/CRT0.O
    # 128-bit soft-float runtime: shipped as a plain object (the on-machine packet
    # linker loops on its symbol table), grow force-links it for CORE users.
    [ -e "$ROOT/build/lib/pl_lib-arm64.o" ] && mcopy -i "$DATAPART" "$ROOT/build/lib/pl_lib-arm64.o" ::SYS/LIB/SOFTFP.O
fi

# LOADSPRITE images are NOT dumped in the root. An image that an example uses
# lives next to that example (e.g. examples/graphics/pic.png -> /EXAMPLES/GRAPHICS/
# PIC.PNG, which imgload.bas loads by bare name from its own directory).
# TrueType fonts for LOADFONT/GTEXT live in /SYS/FONTS - the home the loader
# searches (after the program's own directory), so any program finds a font by a
# bare name. Long names are preserved via VFAT; a short PHILO.TTF alias of the
# bundled Philosopher font gives examples a tidy 8.3 name to load.
mmd -i "$DATAPART" ::SYS/FONTS 2>/dev/null || true
for ttf in "$ROOT"/fonts/*.ttf "$ROOT"/fonts/*.ttc; do
    [ -e "$ttf" ] || continue
    mcopy -o -i "$DATAPART" "$ttf" "::SYS/FONTS/$(basename "$ttf" | tr '[:lower:]' '[:upper:]')"
done
[ -e "$ROOT/fonts/Philosopher-Regular.ttf" ] &&
    mcopy -o -i "$DATAPART" "$ROOT/fonts/Philosopher-Regular.ttf" ::SYS/FONTS/PHILO.TTF

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

