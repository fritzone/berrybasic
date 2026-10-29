#!/usr/bin/env python3
"""Independent validator for the BerryBasiC POD executable format.

Re-derives every checksum and re-checks every structural invariant from the
spec, without sharing any code with tccpod.c.  Exit 0 = valid, prints a report.

    podcheck.py FILE.POD
"""
import sys, struct

MAGIC = b"\x50\x4F\x44\x21\x0D\x0A\x1A\x0A"
CANON = ["MARK", "NEED", "IMAG", "KEYW", "RLOC", "NOTE", "SEAL"]

def crc32c(buf):
    crc = 0xFFFFFFFF
    for b in buf:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 & -(crc & 1)) & 0xFFFFFFFF
            crc &= 0xFFFFFFFF
    return crc ^ 0xFFFFFFFF

def fail(msg):
    print("FAIL:", msg); sys.exit(1)

def main():
    if len(sys.argv) != 2:
        fail("usage: podcheck.py FILE.POD")
    d = open(sys.argv[1], "rb").read()
    ok = []

    # ---- fixed header ----
    if d[:8] != MAGIC: fail("bad magic")
    ok.append("magic")
    (fmt, abi, chunk_count, flags) = struct.unpack_from("<HHHH", d, 8)
    (file_size, image_size, split_off, init_size, entry_off,
     stack_need, heap_need) = struct.unpack_from("<IIIIIII", d, 16)
    (caps,) = struct.unpack_from("<Q", d, 44)
    (build_epoch, payload_crc, header_crc) = struct.unpack_from("<III", d, 52)

    if fmt != 1: fail(f"format_ver {fmt} != 1")
    if file_size != len(d): fail(f"file_size {file_size} != actual {len(d)}")
    ok.append("file_size")
    if crc32c(d[0:60]) != header_crc: fail("header_crc mismatch")
    ok.append("header_crc")

    # structural image invariants
    if split_off % 0x1000 != 0: fail(f"split_off 0x{split_off:x} not page-aligned")
    if init_size > image_size: fail("init_size > image_size")
    if entry_off % 4 != 0: fail("entry_off not 4-aligned")
    is_ext = bool(flags & 1)
    if not is_ext and entry_off >= split_off:
        fail("entry_off not below split (in writable memory)")
    ok.append("image-invariants")

    # ---- walk chunks (forward only) ----
    off = 64
    seen = []
    chunks = {}
    seal_start = None
    while off < len(d):
        tag = d[off:off+4].decode("latin1")
        (size, ccrc) = struct.unpack_from("<II", d, off+4)
        payload = d[off+12:off+12+size]
        if len(payload) != size: fail(f"{tag}: truncated payload")
        if crc32c(payload) != ccrc: fail(f"{tag}: chunk CRC mismatch")
        if tag == "SEAL": seal_start = off
        seen.append(tag)
        chunks[tag] = payload
        off += 12 + size
        off = (off + 3) & ~3            # pad to 4
    ok.append("chunk-crcs")

    if len(seen) != chunk_count:
        fail(f"chunk_count {chunk_count} != walked {len(seen)}")
    ok.append("chunk_count")

    # canonical order (subsequence of CANON)
    order_idx = [CANON.index(t) for t in seen if t in CANON]
    if order_idx != sorted(order_idx):
        fail(f"chunks out of canonical order: {seen}")
    ok.append("canonical-order")

    for must in ("IMAG", "SEAL", "MARK"):
        if must not in chunks: fail(f"missing mandatory chunk {must}")
    if seen[-1] != "SEAL": fail("SEAL is not last")
    ok.append("mandatory-chunks")

    # payload_crc: [64 .. seal_start)
    if crc32c(d[64:seal_start]) != payload_crc: fail("payload_crc mismatch")
    ok.append("payload_crc")

    # SEAL: method, whole_crc over [0, seal_start)
    seal = chunks["SEAL"]
    method = seal[0]
    (whole,) = struct.unpack_from("<I", seal, 4)
    if crc32c(d[0:seal_start]) != whole: fail("SEAL whole_crc mismatch")
    ok.append("seal-whole_crc")

    # IMAG must be exactly init_size
    if len(chunks["IMAG"]) != init_size:
        fail(f"IMAG size {len(chunks['IMAG'])} != init_size {init_size}")
    ok.append("imag-size")

    # RLOC entries must point inside the image and carry a known kind
    nrloc = 0
    if "RLOC" in chunks:
        r = chunks["RLOC"]
        (nrloc,) = struct.unpack_from("<I", r, 0)
        for k in range(nrloc):
            (roff, kind) = struct.unpack_from("<IB", r, 4 + k*8)
            if roff >= image_size: fail(f"RLOC offset 0x{roff:x} outside image")
            if kind not in (0, 1, 2): fail(f"RLOC bad kind {kind}")
        ok.append("rloc")

    # MARK is NUL-terminated key=value text
    mark = {}
    for rec in chunks["MARK"].split(b"\x00"):
        if not rec: continue
        if b"=" in rec:
            k, v = rec.split(b"=", 1)
            mark[k.decode("latin1")] = v.decode("latin1")

    print(f"OK: {sys.argv[1]}  ({file_size} bytes)")
    print(f"  checks passed : {', '.join(ok)}")
    print(f"  kind          : {'extension' if is_ext else 'program'}  flags=0x{flags:x}")
    print(f"  image         : {image_size} (init {init_size}, split 0x{split_off:x}, bss {image_size-init_size})")
    print(f"  entry_off     : 0x{entry_off:x}")
    print(f"  abi/caps      : abi={abi} caps=0x{caps:x}")
    print(f"  chunks        : {seen}")
    print(f"  relocs        : {nrloc}")
    print(f"  seal method   : {method} (0=CRC only)")
    print(f"  MARK          : {mark}")
    if "NEED" in chunks:
        needs = [x.decode('latin1') for x in chunks['NEED'].split(b'\x00') if x]
        print(f"  NEED          : {needs}")
    sys.exit(0)

if __name__ == "__main__":
    main()
