#!/usr/bin/env python3
"""Tokisaki Port -- walk direct callers (bl) of a guest function upward,
annotated with ftrace entry counts, to find where a never-run call chain is
cut off.   .venv/bin/python -I tools/callers.py 0x2AE028 [depth=4]
"""
import bisect, json, os, struct, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
elf = open(os.path.join(ROOT, "game_dump/PS3_GAME/USRDIR/EBOOT.ELF"), "rb").read()
# Every lifted function (OPD entries plus bl targets), from tools/lift.sh.
funcs = sorted(int(l, 16) for l in open(os.path.join(ROOT, "out/lifted_funcs.txt")))
counts = {}
for line in open(os.path.join(ROOT, "logs/ftrace_boot_funcs.txt")):
    n, name = line.split(); counts[int(name[5:], 16)] = int(n)
calls = {}
for o in range(0, 0xbaa45c, 4):
    w = struct.unpack_from(">I", elf, o)[0]
    if (w >> 26) == 18 and (w & 1):
        li = w & 0x03FFFFFC
        if li & 0x02000000: li -= 0x04000000
        a = o + 0x10000
        calls.setdefault(li if (w & 2) else a + li, []).append(a)
# data references to an OPD of the function (virtual / function pointer use)
opds = {}
for o in range(0xe40000, 0xe40000 + 0x15c5e0, 4):
    v = struct.unpack_from(">I", elf, o)[0]
    opds.setdefault(v, []).append(0xe50000 + o - 0xe40000)
def owner(a):
    return funcs[bisect.bisect_right(funcs, a) - 1]
def walk(fn, depth, seen, ind=""):
    print(f"{ind}func_{fn:08X} ran={counts.get(fn, 0)}" + (f"  (opd/ptr at {', '.join(hex(x) for x in opds.get(fn, [])[:3])})" if fn in opds else ""))
    if depth == 0 or fn in seen: return
    seen.add(fn)
    for site in calls.get(fn, [])[:8]:
        print(f"{ind}  <- bl at 0x{site:08X}")
        walk(owner(site), depth - 1, seen, ind + "    ")
walk(int(sys.argv[1], 16), int(sys.argv[2]) if len(sys.argv) > 2 else 4, set())
