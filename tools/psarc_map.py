#!/usr/bin/env python3
"""Tokisaki Port -- map [fsread] positions in a run log to data_ps3.psarc members.

    .venv/bin/python -I tools/psarc_map.py game_dump/PS3_GAME/USRDIR/data_ps3.psarc logs/run.log

The log lines come from PS3_FSLOG_READS=<fd> (ps3recomp ppu_fs.cpp):
    [fsread] fd=3 want=N got=N pos=P
Prints, in first-touch order, each member the title read and how many bytes.
"""
import bisect, struct, sys, zlib

path, log = sys.argv[1], sys.argv[2]
f = open(path, "rb")
hdr = f.read(32)
magic, ver, comp, toc_len, ent_size, n_ent, block, flags = struct.unpack(">4sI4sIIIII", hdr)
assert magic == b"PSAR", magic
toc = f.read(toc_len - 32)
entries = []
for i in range(n_ent):
    e = toc[i * ent_size:(i + 1) * ent_size]
    zidx = struct.unpack(">I", e[16:20])[0]
    length = int.from_bytes(e[20:25], "big")
    offset = int.from_bytes(e[25:30], "big")
    entries.append((offset, length, zidx))
bsz_bytes = 2 if block <= 0x10000 else 3 if block <= 0x1000000 else 4
zsizes_raw = toc[n_ent * ent_size:]
nz = len(zsizes_raw) // bsz_bytes
zsizes = [int.from_bytes(zsizes_raw[i * bsz_bytes:(i + 1) * bsz_bytes], "big") for i in range(nz)]

def read_member(k):
    off, length, zi = entries[k]
    out = bytearray(); f.seek(off)
    while len(out) < length:
        zs = zsizes[zi] or block; zi += 1
        chunk = f.read(zs)
        try: out += zlib.decompress(chunk)
        except zlib.error: out += chunk
    return bytes(out[:length])

names = ["<manifest>"] + read_member(0).decode("utf-8", "replace").split("\n")
starts = sorted((o, i) for i, (o, l, z) in enumerate(entries))
offs = [s[0] for s in starts]
order, got = [], {}
for line in open(log, errors="replace"):
    if not line.startswith("[fsread]"):
        continue
    kv = dict(x.split("=") for x in line.split()[1:])
    pos, n = int(kv["pos"]), int(kv["got"])
    k = bisect.bisect_right(offs, pos) - 1
    if k < 0:
        name = "<toc>"
    else:
        idx = starts[k][1]
        name = names[idx] if idx < len(names) else f"#{idx}"
    if name not in got:
        order.append(name); got[name] = 0
    got[name] += n
for name in order:
    print(f"{got[name]:>10}  {name}")
print(f"{len(order)} members touched", file=sys.stderr)
