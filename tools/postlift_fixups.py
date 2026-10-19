#!/usr/bin/env python3
"""Tokisaki Port -- source fixups applied to the lifted PPU code (recompiled/).

The lift is regenerated from the user's own EBOOT and never committed, so a
fix that has to live in the generated C goes here and is applied after every
lift (tools/lift.sh calls this). Each fixup names the guest function, the
exact text it replaces and why. Idempotent: an applied fixup is skipped.
"""
import pathlib, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
REC = ROOT / "recompiled"

FIXUPS = [
    (
        "func_009A21D0",
        # Sound engine: push a voice onto the free list. When the voice is
        # already the list head, the title calls its error reporter and parks
        # in `b .` (a deliberate fatal stop). The double release comes from the
        # campaign-start sound teardown, which this port still has to force
        # (src/kz2_hle.cpp, sound watchdog). Ignoring the duplicate is harmless:
        # the node is on the list exactly once either way. Without this, the
        # empty infinite loop is undefined behaviour in C++ and clang turned it
        # into a call through a null trampoline.
        """loc_009A2270:
        ctx->lr = 0x009A2274; func_00994530(ctx); DRAIN_TRAMPOLINE(ctx);
        /* nop */;
        ctx->gpr[3] = vm_read32(ctx->gpr[2] + -0x4780);
        ctx->lr = 0x009A2280; func_009A17B0(ctx); DRAIN_TRAMPOLINE(ctx);
        /* nop */;
loc_009A2284:
        goto loc_009A2284;
}""",
        """loc_009A2270:
        ctx->lr = 0x009A2274; func_00994530(ctx); DRAIN_TRAMPOLINE(ctx);
        /* kz2 postlift: voice already at the free-list head -- skip the fatal stop */
        ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0x90);
        ctx->gpr[31] = _cs_31;
        ctx->lr = ctx->gpr[0];
        ctx->gpr[1] = ctx->gpr[1] + (int64_t)(0x80);
        return;
}""",
    ),
]


def main() -> int:
    files = sorted(REC.glob("ppu_recomp_*.cpp"))
    if not files:
        print("[postlift] no lifted sources in recompiled/", file=sys.stderr)
        return 1
    status = 0
    for fn, old, new in FIXUPS:
        hit = False
        for f in files:
            src = f.read_text()
            if f"void {fn}(ppu_context* ctx)" not in src:
                continue
            if new in src:
                print(f"[postlift] present  {fn} ({f.name})")
                hit = True
            elif old in src:
                f.write_text(src.replace(old, new, 1))
                print(f"[postlift] applied  {fn} ({f.name})")
                hit = True
        if not hit:
            print(f"[postlift] MISSING  {fn}", file=sys.stderr)
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
