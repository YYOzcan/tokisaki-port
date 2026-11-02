#!/usr/bin/env python3
"""Tokisaki Port -- insert the native gates of tools/spu_native_gates.tsv into the lifted SPU job modules.

A gate is the first statement of a lifted job function (after Project Pyrrhus's tools/spu/lift_spu.py,
native_gates(); MIT, see THIRD_PARTY_NOTICES.md):

    if (g_kz2_native_gate[slot] && g_kz2_native_gate[slot](ctx)) SPU_RET(ctx);

g_kz2_native_gate[] is filled at start by src/spu_native/kz2_native_jobs.c (KZ2_NATIVE_JOB_* flags, Pyrrhus's
defaults); a gate function returns 1 when its native kernel did the whole subroutine, 0 to run the lifted body.
A function the lift made skippable keeps that skip first. A row with a cont address is a "continue at" gate: on 1
it branches to that address's lifted function the way the lifted code's own branches do. Idempotent (a gate already in place is left alone); the lift is regenerated
from the user's own EBOOT, so tools/lift_spu.sh runs this after lifting.

    tools/spu_native_gates.py [spu_recompiled dir]
"""
import os, pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TSV = ROOT / "tools" / "spu_native_gates.tsv"
DECL = "extern int (*g_kz2_native_gate[])(spu_context*);   /* native gates: tools/spu_native_gates.tsv */\n"
SKIP = "        if (g_spu_skip_marked) { SPU_RET(ctx); }   /* --skippable-funcs */\n"


def main():
    spu = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "spu_recompiled"
    rows = {}
    for line in open(TSV):
        if line.startswith("#") or not line.strip():
            continue
        f = line.rstrip("\n").split("\t")
        cont = int(f[4], 16) if len(f) > 4 and f[4].strip() else 0
        rows.setdefault(f[0], []).append((int(f[1], 16), int(f[2]), f[3], cont))
    added = present = 0
    for sha, gates in sorted(rows.items()):
        dirs = sorted(d for d in os.listdir(spu) if re.fullmatch(r"jobmod_%s_e[0-9A-Fa-f]+" % sha, d))
        if len(dirs) != 1:
            sys.exit(f"[native-gates] module {sha}: {len(dirs)} lifted images, expected one")
        name = dirs[0]
        p = spu / name / "spu_recomp.c"
        t = p.read_text()
        n0 = added
        for addr, slot, job, cont in gates:
            head = f"void {name}_spu_func_{addr:08X}(spu_context* ctx) {{\n"
            if head not in t:
                sys.exit(f"[native-gates] no lifted function {name} 0x{addr:X}")
            mark = f"/* native gate {slot}: "
            if mark in t:
                present += 1
                continue
            then = "SPU_RET(ctx);"
            if cont:
                fn = f"{name}_spu_func_{cont:08X}"
                if f"void {fn}(spu_context* ctx) {{\n" not in t:
                    sys.exit(f"[native-gates] continue address 0x{cont:X} of {name} 0x{addr:X} is no lifted function")
                then = f"{{ ctx->pc = 0x{cont:X}; g_spu_trampoline_fn = {fn}; return; }}"
            gate = (f"        if (__builtin_expect(g_kz2_native_gate[{slot}] != 0, 0) && g_kz2_native_gate[{slot}](ctx)) "
                    f"{then}   {mark}{job} 0x{addr:X} (tools/spu_native_gates.tsv) */\n")
            # A function the lift made skippable (lift_spu.sh JOBMOD_SKIPPABLE: the depth-of-field
            # blur this port leaves out) keeps its skip first: the gate goes after it.
            at = head + SKIP if head + SKIP in t else head
            t = t.replace(at, at + gate, 1)
            added += 1
        if added != n0:
            if DECL not in t:
                t = t.replace('#include "spu_recomp.h"\n', '#include "spu_recomp.h"\n' + DECL, 1)
                if DECL not in t:
                    sys.exit(f"[native-gates] {name}: no #include \"spu_recomp.h\" to put the declaration after")
            p.write_text(t)
    print(f"[native-gates] {added} inserted, {present} already present")


if __name__ == "__main__":
    main()
