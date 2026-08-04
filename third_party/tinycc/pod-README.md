# POD output for TinyCC

This tree adds a native output format to tcc: the **POD executable** used by the
BerryBasiC machine (see `doc/The POD Executable Format`). A POD is one flat,
position-independent image split once into read-execute and read-write halves,
wrapped by a 64-byte checksummed header and a short run of chunks (`MARK`,
`NEED`, `IMAG`, `KEYW`, `RLOC`, `NOTE`, `SEAL`).

## Using it

```sh
tcc -pod hello.c -o HELLO.POD          # a program POD (has pod_main)
tcc -pod hypot.c -o HYPOT.POD          # an extension POD (registers keywords)
python3 tests/podcheck.py HELLO.POD    # independent validator (re-derives every CRC)
make test-pod                          # build + validate the two examples/
```

`-pod` implies a static, freestanding link at text address 0 with a 4 KiB RX/RW
split, so no other flags are required. A POD source `#include <pod.h>` and
declares its manifest with the `POD_*` macros:

```c
#include <pod.h>
POD_NAME("hello")
POD_NEEDS(CAP_CONSOLE, "CONSOLE=prints a greeting")
int pod_main(const PodServices *svc, int argc, const char *const *argv) {
    static const char m[] = "hi\n"; svc->puts(m, sizeof m - 1); return 0;
}
```

Target: **AArch64** (`arm64-tcc`) is the BerryBasiC platform. The format is
arch-neutral and the writer also works for the native target, which is handy for
testing on the build host.

## What was added

| File | Change |
|------|--------|
| `include/pod.h`   | Author header: `PodServices`, `CAP_*`, `pod_main`, and the manifest macros (`POD_NAME`/`POD_NEEDS`/`POD_KEYWORD`/...) that emit records into a `.pod.desc` section. |
| `tccpod.c`        | The writer: flattens the laid-out image, computes `split_off`/`init_size`/`image_size`/`entry_off`, reads the manifest, emits the chunks and every CRC-32C, and harvests absolute relocations for `RLOC`. |
| `tccelf.c`        | Dispatches to `tcc_output_pod` from `tcc_write_elf_file`; calls `pod_collect_relocs` after the GOT is filled (before `.rela` is reordered away). |
| `arm64-gen.c`     | For POD output, addresses local symbols with a PC-relative `ADRP+ADD` pair instead of the GOT, so POD code is genuinely position independent. |
| `libtcc.c`, `tcc.h`, `tcc.c` | The `-pod` option, `TCC_OUTPUT_FORMAT_POD`, the `pod` state, and the implied link settings. |
| `examples/pod_hello.c`, `examples/pod_hypot.c`, `tests/podcheck.py` | A program POD, an extension POD, and a standalone validator. |

## How position independence is handled

tcc binds the image as if it loaded at 0. AArch64 control flow is PC-relative
and needs no fixup; with the `arm64-gen.c` change above, so is local data access.
The only things bound to the base are **absolute data pointers** (pointer
initialisers, function tables), and each of those is recorded in the `RLOC`
chunk for the loader to add the load base to. A POD that has no such pointers
carries an empty `RLOC` and is fully position independent (the two examples are).

## Notes / limitations

- **Seal method 0** (CRC-32C only) is emitted; the header and payload are also
  CRC-checked. SHA-256/signature seals (methods 1/2) are room the format leaves
  but this writer does not yet fill.
- The image is padded to page boundaries at each RX/RW permission change, so a
  minimal POD is ~1-2 pages. This is inherent to the single page-aligned split.
- On non-AArch64 targets the GOT path is unchanged, so PODs built there may
  carry more `RLOC` entries; AArch64 is the intended target.
