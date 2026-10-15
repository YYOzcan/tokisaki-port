#!/usr/bin/env python3
"""Tokisaki Port -- list a guest function's direct callees with their entry
counts from an ftrace run (logs/ftrace_boot_funcs.txt: "<count> func_XXXXXXXX").

    .venv/bin/python -I tools/callees.py 0x2BA320 [logs/ftrace_boot_funcs.txt]
"""
import bisect, json, os, struct, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
elf = open(os.path.join(ROOT, "game_dump/PS3_GAME/USRDIR/EBOOT.ELF"), "rb").read()
funcs = sorted({int(f["start"], 16) if isinstance(f["start"], str) else f["start"]
                for f in json.load(open(os.path.join(ROOT, "out/EBOOT.functions.json")))})
counts = {}
for line in open(sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "logs/ftrace_boot_funcs.txt")):
    n, name = line.split()
    counts[int(name[5:], 16)] = int(n)
start = int(sys.argv[1], 16)
k = bisect.bisect_right(funcs, start)
end = funcs[k] if k < len(funcs) else start + 0x4000
pc = start
while pc < end:
    w = struct.unpack_from(">I", elf, pc - 0x10000)[0]
    if (w >> 26) == 18 and (w & 1):
        li = w & 0x03FFFFFC
        if li & 0x02000000: li -= 0x04000000
        dst = li if (w & 2) else pc + li
        print(f"  0x{pc:08X}: bl func_{dst:08X}  ran {counts.get(dst, 0)}")
    if (w >> 26) == 19 and ((w >> 1) & 0x3FF) == 528 and (w & 1):
        print(f"  0x{pc:08X}: bctrl")
    pc += 4
print(f"func_{start:08X} itself ran {counts.get(start, 0)}")
