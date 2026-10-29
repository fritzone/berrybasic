#!/usr/bin/env bash
# podcc.sh - compile a C program (that uses the pod-libc) into a POD.
#   tools/podcc.sh SRC.c OUT.POD [extra tcc args...]
# Links the program with the pod-libc runtime (crt0 + stdio/stdlib/string/...)
# so it can use ordinary <stdio.h>/<stdlib.h>/<string.h> on the machine.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TCC="$ROOT/third_party/tinycc"
SRC="$1"; OUT="$2"; shift 2
"$TCC/arm64-tcc" -B"$TCC" -pod \
    -nostdinc -I"$ROOT/podlib/include" -I"$TCC/include" \
    "$SRC" \
    "$ROOT/podlib/src/crt0.c" "$ROOT/podlib/src/stdio.c" "$ROOT/podlib/src/stdlib.c" \
    "$ROOT/podlib/src/string.c" "$ROOT/podlib/src/misc.c" "$ROOT/podlib/src/extra.c" \
    "$ROOT/podlib/src/math.c" "$ROOT/podlib/src/time.c" \
    "$ROOT/podlib/src/setjmp.S" "$TCC/lib/lib-arm64.c" \
    "$@" -o "$OUT"
