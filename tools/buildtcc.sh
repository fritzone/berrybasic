#!/usr/bin/env bash
# buildtcc.sh - build the Tiny C Compiler itself as a BerryBasiC POD.
#
# Compiles tinycc (libtcc.c ONE_SOURCE + the tcc.c driver) and its soft-float128
# runtime against the pod-libc, then links the lot with `tcc -pod` into TCC.POD.
# The result runs on the machine and can compile C (including `tcc -pod`).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TCC="$ROOT/third_party/tinycc"
LIB="$ROOT/podlib"
OUT="${1:-$ROOT/build/sys/tcc.POD}"
O="$ROOT/build/tcc-obj"
mkdir -p "$O" "$(dirname "$OUT")"

CC="$TCC/arm64-tcc"
INC="-nostdinc -I$LIB/include -I$TCC/include -I$TCC"
# On the machine, the compiler's default header path is /sys/include (where the
# SD image drops pod.h and the pod-libc headers), so `#include <pod.h>` just works.
DEF="-DTCC_TARGET_ARM64 -DTCC_POD -DCONFIG_TCC_STATIC -DCONFIG_TCC_SEMLOCK=0 \
     -DCONFIG_TCCDIR=\"/sys\" -DCONFIG_TCC_SYSINCLUDEPATHS=\"/sys/include\" \
     -DCONFIG_TCC_LIBPATHS=\"/sys/lib\" -DCONFIG_TCC_ELFINTERP=\"\""

echo "[tcc-pod] compiling tinycc core (libtcc.c, ONE_SOURCE)"
"$CC" -c $INC $DEF -DONE_SOURCE=1 "$TCC/libtcc.c" -o "$O/libtcc.o"
echo "[tcc-pod] compiling driver (tcc.c)"
"$CC" -c $INC $DEF -DONE_SOURCE=0 "$TCC/tcc.c"    -o "$O/tcc.o"
echo "[tcc-pod] compiling soft-float128 runtime (lib-arm64.c)"
"$CC" -c $INC $DEF "$TCC/lib/lib-arm64.c"         -o "$O/lib-arm64.o"
echo "[tcc-pod] compiling manifest"
"$CC" -c -I"$TCC/include" "$TCC/tcc_pod.c"        -o "$O/tcc_pod.o"

echo "[tcc-pod] compiling pod-libc"
for f in crt0 stdio stdlib string misc extra; do
    "$CC" -c $INC "$LIB/src/$f.c" -o "$O/$f.o"
done
"$CC" -c "$LIB/src/setjmp.S" -o "$O/setjmp.o"

echo "[tcc-pod] linking -> $OUT"
"$CC" -B"$TCC" -pod \
    "$O/tcc.o" "$O/libtcc.o" "$O/lib-arm64.o" "$O/tcc_pod.o" \
    "$O/crt0.o" "$O/stdio.o" "$O/stdlib.o" "$O/string.o" "$O/misc.o" "$O/extra.o" "$O/setjmp.o" \
    -o "$OUT"
echo "[tcc-pod] done: $OUT"
