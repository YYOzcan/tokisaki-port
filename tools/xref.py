#!/usr/bin/env python3
"""Tokisaki Port -- cross-references to a guest address: data words equal to
it, and lis/addi (or lis/ori) pairs that build it in code.
    .venv/bin/python -I tools/xref.py 0xE82398
"""
import bisect, json, os, struct, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
elf = open(os.path.join(ROOT, "game_dump/PS3_GAME/USRDIR/EBOOT.ELF"), "rb").read()
# Every lifted function (OPD entries plus bl targets), from tools/lift.sh.
funcs = sorted(int(l, 16) for l in open(os.path.join(ROOT, "out/lifted_funcs.txt")))
va = int(sys.argv[1], 16)
for o in range(0xe40000, 0xe40000 + 0x15c5e0, 4):
    if struct.unpack_from(">I", elf, o)[0] == va:
        print(f"data word at 0x{0xe50000 + o - 0xe40000:08X}")
for o in range(0, 0xbaa45c, 4):
    w = struct.unpack_from(">I", elf, o)[0]
    op = w >> 26
    if op in (14, 24, 32, 12, 13):
        rA = (w >> 16) & 31; imm = w & 0xFFFF
        if op in (14, 32, 12, 13) and imm & 0x8000: imm -= 0x10000
        for k in range(1, 12):
            p = struct.unpack_from(">I", elf, o - 4 * k)[0]
            if (p >> 26) == 15 and ((p >> 16) & 31) == 0 and ((p >> 21) & 31) == rA:
                if ((p & 0xFFFF) << 16) + imm == va:
                    a = o + 0x10000
                    print(f"code at 0x{a:08X} in func_{funcs[bisect.bisect_right(funcs, a) - 1]:08X}")
                break
