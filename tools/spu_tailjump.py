#!/usr/bin/env python3
"""Tokisaki Port -- direct branches between lifted SPU functions as tail calls.

The lifter ends a function that branches into another one with
`g_spu_trampoline_fn = f; return;`: back to the drain loop, which calls f.
Every such hop returned through an indirect call, and the out-of-line drain
was ~2.7 ms of the SPURS lanes a frame. This rewrites them to
`SPU_TAILJ(ctx, f);` (runtime/spu/spu_context.h), which calls f in place when
the drain loop would do nothing else between the two. Indirect branches
(spu_indirect_branch) are left as they are.
    tools/spu_tailjump.py <spu_recompiled dir>    (idempotent)"""
import pathlib, re, sys

PAT = re.compile(r'g_spu_trampoline_fn = ((?!spu_indirect_branch\b)[A-Za-z_]\w*_spu_func_[0-9A-Fa-f]+); return;')
root = pathlib.Path(sys.argv[1])
total = 0
for f in sorted(root.glob('*/spu_recomp*.c')):
    s = f.read_text()
    n = len(PAT.findall(s))
    if not n:
        continue
    f.write_text(PAT.sub(r'SPU_TAILJ(ctx, \1);', s))
    total += n
print(f"spu_tailjump: {total} direct branches as tail calls")
