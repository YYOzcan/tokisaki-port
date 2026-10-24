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
    # Entity-update fiber jobs (docs/PROGRESS.md section 37, src/kz2_fiber.cpp):
    # the title's hand-written context switch cannot run as lifted C, so the
    # batch runner, the job end and the dependency wait go to host code.
    (
        "func_00072438",
        "void func_00072438(ppu_context* ctx) {\n",
        'extern "C" void kz2_fiber_run_batch(ppu_context*);\n'
        "void func_00072438(ppu_context* ctx) {\n"
        "        /* kz2 postlift: fiber batch on the host */ kz2_fiber_run_batch(ctx); return;\n",
    ),
    (
        "func_00072760",
        "void func_00072760(ppu_context* ctx) {\n",
        'extern "C" void kz2_fiber_job_end(ppu_context*);\n'
        "void func_00072760(ppu_context* ctx) {\n"
        "        /* kz2 postlift: fiber job end on the host */ kz2_fiber_job_end(ctx); return;\n",
    ),
    (
        "func_00072600",
        "void func_00072600(ppu_context* ctx) {\n",
        'extern "C" void kz2_fiber_wait(ppu_context*);\n'
        "void func_00072600(ppu_context* ctx) {\n"
        "        /* kz2 postlift: fiber wait on the host */ kz2_fiber_wait(ctx); return;\n",
    ),
    # The main thread's wait for a job batch's progress counters: the host
    # version pauses between passes instead of re-reading flat out on a core
    # an SPU worker shares (src/kz2_hle.cpp, kz2_spu_progress_wait).
    (
        "func_003BD660",
        "void func_003BD660(ppu_context* ctx) {\n",
        'extern "C" int kz2_spu_progress_wait(ppu_context*);\n'
        "void func_003BD660(ppu_context* ctx) {\n"
        "        /* kz2 postlift: progress wait on the host */ if (kz2_spu_progress_wait(ctx)) return;\n",
    ),
    # Direct mouse look (src/kz2_mouse.cpp): the player aim update takes the
    # mouse motion itself once per frame -- marked where the local player's
    # look input is read, applied where the new pitch and the turn input are
    # final -- instead of through the right stick's curve and ramp.
    (
        "func_0016E620",
        "void func_0016E620(ppu_context* ctx) {\n",
        'extern "C" void kz2_mouse_look_mark(ppu_context*);\n'
        'extern "C" void kz2_mouse_look_apply(ppu_context*);\n'
        "void func_0016E620(ppu_context* ctx) {\n",
    ),
    (
        "func_0016E620",
        '        ctx->lr = 0x0016E794; func_00080A78(ctx); DRAIN_TRAMPOLINE(ctx);\nloc_0016E794:\n',
        '        ctx->lr = 0x0016E794; func_00080A78(ctx); DRAIN_TRAMPOLINE(ctx);\n'
        "        /* kz2 postlift: mouse look, local player */ kz2_mouse_look_mark(ctx);\n"
        "loc_0016E794:\n",
    ),
    (
        "func_0016E620",
        'loc_0016F400:\n        ctx->gpr[3] = (int64_t)(int32_t)(0xA0);\n        { uint64_t _a = ctx->gpr[1]; uint64_t _r = _a + (uint64_t)(int64_t)(0xA0); ctx->gpr[4] = _r; ctx->xer = (ctx->xer & ~(1u << 29)) | ((_r < _a) ? (1u << 29) : 0u); }\n        { uint64_t ea = (ctx->gpr[3] + ctx->gpr[1]); ea &= ~3ULL; { uint32_t _e; memcpy(&_e, (const uint8_t*)&ctx->vr[31] + (ea & 15), 4); VM_RAW_ST(uint32_t, ea, _e); } }\n',
        'loc_0016F400:\n        ctx->gpr[3] = (int64_t)(int32_t)(0xA0);\n        { uint64_t _a = ctx->gpr[1]; uint64_t _r = _a + (uint64_t)(int64_t)(0xA0); ctx->gpr[4] = _r; ctx->xer = (ctx->xer & ~(1u << 29)) | ((_r < _a) ? (1u << 29) : 0u); }\n        { uint64_t ea = (ctx->gpr[3] + ctx->gpr[1]); ea &= ~3ULL; { uint32_t _e; memcpy(&_e, (const uint8_t*)&ctx->vr[31] + (ea & 15), 4); VM_RAW_ST(uint32_t, ea, _e); } }\n'
        "        /* kz2 postlift: mouse look */ kz2_mouse_look_apply(ctx);\n",
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
