#!/usr/bin/env python3
"""Tokisaki Port -- extract the WWS SPU job modules embedded in EBOOT.ELF.

Killzone 2 links its SPURS job binaries (WWS "C0DEC0DE" modules) straight
into the PPU image instead of shipping them in an SDATA archive, so
ps3recomp's extract_jobmods.py has nothing to open. A module is laid out as

    +0x00  4x ila             (code buffer setup)
    +0x10  be32 entryOffset
    +0x14  be32 imageSize     (initialised image + a fixed 32-byte bss tail)
    +0x20  0xC0DEC0DE

and is written as jobmod_<sha1[:12]>_e<entry>.bin, the naming
lift_jobmods.py expects.

    .venv/bin/python -I tools/extract_jobmods_elf.py game_dump/.../EBOOT.ELF out/jobmods
"""
import hashlib, os, struct, sys

elf, outdir = sys.argv[1], sys.argv[2]
data = open(elf, "rb").read()
os.makedirs(outdir, exist_ok=True)

# imageSize counts the module's whole LS footprint (bss included), so it can
# run past the next module in the file. Clamp each image to the next one.
hits = []
p = 0
while (p := data.find(b"\xc0\xde\xc0\xde", p)) >= 0:
    hits.append(p - 0x20)
    p += 4

seen = {}
pos = 0
while True:
    pos = data.find(b"\xc0\xde\xc0\xde", pos)
    if pos < 0:
        break
    start = pos - 0x20
    pos += 4
    if start < 0 or data[start] not in (0x42, 0x43):
        continue
    entry, size = struct.unpack_from(">II", data, start + 0x10)
    if not (0x30 <= size <= 0x3C000) or entry >= size:
        continue
    nxt = min([h for h in hits if h > start] + [len(data)])
    mod = data[start:min(start + size - 32, nxt)]
    h = hashlib.sha1(mod).hexdigest()
    if h not in seen:
        seen[h] = (start, entry, mod)

for h, (start, entry, mod) in sorted(seen.items(), key=lambda kv: kv[1][0]):
    name = f"jobmod_{h[:12]}_e{entry:04X}.bin"
    open(os.path.join(outdir, name), "wb").write(mod)
    print(f"  file+0x{start:08X} (guest 0x{start + 0x10000:08X})  {len(mod):6d} B  entry +0x{entry:04X}  {name}")
print(f"{len(seen)} unique job module(s) -> {outdir}")
