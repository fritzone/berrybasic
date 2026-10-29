#!/usr/bin/env bash
# builddoom.sh - compile the DOOM port (third_party/DOOM/linuxdoom-1.10) into a
# POD, linked with the pod-libc. Output: build/sys/DOOM.POD (arg 1 overrides).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TCC="$ROOT/third_party/tinycc"
POD="$ROOT/podlib"
DOOM="$ROOT/third_party/DOOM/linuxdoom-1.10"
OUT="${1:-$ROOT/build/sys/DOOM.POD}"
# Make OUT absolute BEFORE we cd into the source dir: the Makefile passes a path
# relative to the repo root ($@ = build/sys/DOOM.POD), but tcc's -o below runs
# after 'cd "$DOOM"', where that same relative path would resolve under the DOOM
# source dir (which has no build/sys) and fail with "could not write ...".
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
mkdir -p "$(dirname "$OUT")"
cd "$DOOM"
"$TCC/arm64-tcc" -B"$TCC" -pod -nostdinc \
    -Icompat -I"$POD/include" -I"$TCC/include" -I. -DNORMALUNIX \
    *.c \
    "$POD/src/crt0.c" "$POD/src/stdio.c" "$POD/src/stdlib.c" "$POD/src/string.c" \
    "$POD/src/misc.c" "$POD/src/extra.c" "$POD/src/math.c" "$POD/src/time.c" \
    "$POD/src/graphics.c" "$POD/src/setjmp.S" "$TCC/lib/lib-arm64.c" "$TCC/lib/alloca.S" \
    -o "$OUT"
echo "[builddoom] -> $OUT ($(stat -c%s "$OUT") bytes)"
