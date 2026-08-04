#!/usr/bin/env python3
"""Independent validator for the BerryBasiC seed-packet (.PKT) format.
Re-derives every CRC and re-checks the structure. Exit 0 = valid."""
import sys, struct

MAGIC = b"\x50\x4B\x54\x21\x0D\x0A\x1A\x0A"

def crc32c(buf):
    crc = 0xFFFFFFFF
    for b in buf:
        crc ^= b
        for _ in range(8):
            crc = ((crc >> 1) ^ (0x82F63B78 & -(crc & 1))) & 0xFFFFFFFF
    return crc ^ 0xFFFFFFFF

def fail(m): print("FAIL:", m); sys.exit(1)

d = open(sys.argv[1], "rb").read()
if d[:8] != MAGIC: fail("bad magic")
fmt, flags = struct.unpack_from("<HH", d, 8)
mcnt, scnt, npool, fsize, hcrc = struct.unpack_from("<IIIII", d, 12)
if fmt != 1: fail(f"format {fmt}")
if fsize != len(d): fail(f"file_size {fsize} != {len(d)}")
if crc32c(d[0:28]) != hcrc: fail("header_crc mismatch")
ok = ["magic", "file_size", "header_crc"]

# members
members = []
mo = 32
for i in range(mcnt):
    r = d[mo:mo+32]
    name_off, data_off, data_size, mcrc = struct.unpack_from("<IIII", r, 0)
    caps = struct.unpack_from("<Q", r, 16)[0]
    name = d[name_off:d.index(b"\0", name_off)].decode()
    body = d[data_off:data_off+data_size]
    if crc32c(body) != mcrc: fail(f"member {name} crc mismatch")
    if body[:4] != b"\x7fELF": fail(f"member {name} is not ELF")
    members.append((name, data_size, caps))
    mo += 32
ok.append("member-crcs")

# symbols (must be sorted, member index in range)
so = 32 + mcnt*32
prev = ""
syms = []
for i in range(scnt):
    r = d[so:so+20]
    name_off, member, kind = struct.unpack_from("<IIB", r, 0)
    name = d[name_off:d.index(b"\0", name_off)].decode()
    if name < prev: fail(f"symbols not sorted: {prev} then {name}")
    if member >= mcnt: fail(f"symbol {name} bad member {member}")
    syms.append((name, member, "T" if kind == 0 else "D"))
    prev = name
    so += 20
ok.append("symbols-sorted")
if npool != 32 + mcnt*32 + scnt*20: fail("name_pool_off wrong")
ok.append("namepool-off")

print(f"OK: {sys.argv[1]}  ({fsize} bytes)")
print(f"  checks : {', '.join(ok)}")
print(f"  members: {[(n,s,hex(c)) for n,s,c in members]}")
print(f"  symbols: {syms}")
