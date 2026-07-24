# BerryBasiC

**A bare-metal BASIC for the Raspberry Pi 4.** No operating system, no shell, no runtime: the Pi boots straight into a `>` prompt in about a second, and the whole machine is yours.

***BerryBasiC *** is a modern, BBC-flavoured BASIC that runs as the *only* program on the hardware. It brings back the directness the 8-bit home computers had (type a statement, watch it happen, poke a pin, light a pixel) on hardware that can actually keep up. It is not nostalgia for old syntax. It is nostalgia for **transparency**: a machine small enough that one person can understand it from the first instruction to the last pixel.

BerryBasiC is a modern, BBC-flavoured BASIC that runs as the *only* program on the hardware. It brings back the directness the 8-bit home computers had — type a statement, watch it happen, poke a pin, light a pixel — on hardware that can actually keep up. It is not nostalgia for old syntax. It is nostalgia for **transparency**: a machine small enough that one person can understand it from the first instruction to the last pixel.

## Features

**A structured language, not a museum piece**

- Named `PROC`/`FN` with parameters, `LOCAL` variables and recursion
- Block `IF`/`ELSE`/`ENDIF`, `FOR`, `REPEAT`, `WHILE`, `CASE`, `EXIT`, `CONTINUE`
- `TRY`/`CATCH` error handling, `EVAL`/`EXEC` for dynamic code
- Dictionaries, lists and trees; records; a full string library
- Reusable libraries via `IMPORT` — each module keeps its own line-number space

**It owns the hardware**

- Graphics: BBC-style `PLOT`/`MOVE`/`DRAW`, shapes, flood fill, 24-bit truecolour
- Sprites with alpha, double buffering, render-to-sprite, tilemaps, TrueType text
- Four-channel sound, USB keyboard and mouse, multiple keyboard layouts
- GPIO, I²C, edge detection — `PIN 17, 1` lights an LED, no driver, no permissions
- Files and directories on the SD card, opened as channels
- An event system: `ON TIMER`, `ON PIN`, `ON MOUSE`

**Native seeds — an escape hatch to full speed**

- Compile C to a position-independent AArch64 blob, load it with `SEED`, call it with `CALL`
- A versioned service ABI: printing, keyboard, variables, arrays, files, GPIO, graphics
- A freestanding seed libc (`malloc`, `qsort`, `string.h`, `ctype.h`) over a private heap
- Seeds can register **new language keywords** and are autoloaded at boot

```basic
10 MODE 1
20 FOR RD = 400 TO 1 STEP -1
30   A = (400 - RD) / 400 * 2 * PI
40   GCOL 128 + 127*SIN(A), 128 + 127*SIN(A+2.0944), 128 + 127*SIN(A+4.1888)
50   CIRCLE FILL 640, 512, RD
60 NEXT
```

## Quick start

### Build

You need a Linux (or WSL2) environment with an AArch64 cross-toolchain:

```bash
sudo apt install build-essential gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
                 make cmake git dosfstools qemu-system-arm

git clone https://github.com/fritzone/berrybasic
cd berrybasic

make            # build/kernel8.img  - the bare-metal image
make seeds      # build/seeds/*.sed  - the native extension blobs
make host       # build/basic_host   - the interpreter, running on your desktop
```

> **On Windows:** use WSL2 with the packages above, and clone into your Linux home directory (`~`), not `/mnt/c` — the Windows filesystem doesn't support the permission operations git needs, and builds there are far slower.

### Try it without hardware

The fastest way in is the host build — the same interpreter, with desktop backends:

```bash
./build/basic_host
```

Or run the real kernel under emulation:

```bash
make run        # QEMU, -M raspi4b, with USB keyboard and mouse
```

### Run it on a real Pi 4

```bash
make sdimage    # builds berrybasic-sd.img (firmware + kernel + seeds)
make flash      # interactively flash it to a removable card
```

Then put the card in a Pi 4, connect HDMI and a USB keyboard, and power on. See [`doc/README-realhw.md`](https://claude.ai/chat/doc/README-realhw.md) for wiring and troubleshooting.

### Configure

```bash
make config     # pick screen resolution, font and keyboard layout
make newseed NAME=blur   # scaffold a new native seed
make test       # Catch2 unit tests for the interpreter
```

## A taste of the language

**Blink an LED on GPIO 17** — physical computing in five lines:

```basic
10 PINMODE 17, OUTPUT
20 REPEAT
30   PIN 17, 1 : TIME = 0 : REPEAT : UNTIL TIME > 50
40   PIN 17, 0 : TIME = 0 : REPEAT : UNTIL TIME > 50
50 UNTIL INKEY(0) <> -1
```

**Smooth, flicker-free animation** with double buffering:

```basic
10 BUFFER ON
20 x = 0
30 REPEAT
40   WAIT : CLG
50   CIRCLE FILL x, 512, 60
60   FLIP
70   x = (x + 8) MOD 1280
80 UNTIL INKEY(0) <> -1
90 BUFFER OFF
```

**Call native code** when BASIC isn't fast enough:

```basic
10 DIM A(999)
20 FOR I = 0 TO 999 : A(I) = I : NEXT
30 SEED H, "SUMARR.SED"
40 PRINT "sum = "; CALL(H, "A")
```

```c
#include "seed.h"

SEED_EXPORT(seed) {
    int len = 0;
    double *a = svc->num_array("A", &len);
    double s = 0;
    for (int i = 0; i < len; i++) s += a[i];
    return s;
}
```

There are several **BASIC examples** in [`examples/`](https://claude.ai/chat/examples/) — graphics demos, Mandelbrot, fractals, sound, GPIO, I²C scanning, file I/O, events, collections — and some **seed examples** in [`seed/examples/`](https://claude.ai/chat/seed/examples/).

---

## ## Status and contributing

BerryBasiC is an actively developed hobby project. Issues, ideas and pull requests are welcome — especially example programs, seeds, and hardware support.

If you're hacking on the interpreter, `make host` plus `make test` gives a fast edit-build-test loop with no emulator in the way.

## License

GPL-3.0. See [LICENSE](https://claude.ai/chat/LICENSE).

Vendored third-party code (`stb_image`, `stb_truetype`) lives in `third_party/` under its own terms.
