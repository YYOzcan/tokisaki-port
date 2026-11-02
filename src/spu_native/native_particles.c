/* From Project Pyrrhus by undecuple (https://github.com/undecuple/project-pyrrhus, port/kz2/spu/native_particles.c at 218f660),
 * MIT License, Copyright (c) 2026 undecuple -- see THIRD_PARTY_NOTICES.md. Tokisaki changes are marked "tokisaki:". */
/* native_particles.c -- Particles.VertexJob (job_00cd7180, image 201) native code: the .bss clear (on by default,
 * KZ2_NATIVE_JOB_VJCLEAR=0 turns it off) and the memcpy (on by default since an earlier change, KZ2_NATIVE_JOB_VJCOPY=0 turns it off).
 *
 * The job's crt0 .bss clear, the first "continue at" gate (re/spu/native_gates.tsv column 7): the WWS job crt0 that
 * 26 of the 36 job images share zeroes its .bss at every job start (@0x406C ila r3 = start, @0x4070 ila r4 = end,
 * @0x4074 clgt + brnz to the loop, which branches back to @0x407C). VertexJob's .bss is LS 0x1B980-0x288B0 (53 040
 * bytes, 3315 quadwords: 6.6 % of the job's host samples in an earlier change's profile); the next largest is 1664 bytes. The
 * clear is no subroutine (the loop's exit is `br 0x407C`, not a return), so its gate continues at 0x407C instead of
 * returning to r0.
 *
 *   @0x411C  hbrr            (hint, nothing)
 *   @0x4120  il    r5, 0
 *   @0x4124  stqx  r5, r3, r126        LS[(r3 + r126) & 0x3FFF0] = 0           (r126 = the image's load offset)
 *   @0x4128  ai    r3, r3, 16          every lane
 *   @0x412C  clgt  r6, r4, r3          every lane
 *   @0x4130  brnz  r6, 0x4124          on the preferred word
 *   @0x4134  br    0x407C
 *
 * The native code leaves everything as the loop does: the quadwords zeroed, r3 advanced by 16 per store in every lane,
 * r5 = 0, r6 = clgt(r4, r3) per lane, and the gate line hops to 0x407C. It declines (the lifted loop runs) when
 * the preferred words are not start < end (the do-while's single store for start >= end, never reached from 0x4078)
 * or the range is larger than the local store. */
#include "kz2_native_jobs.h"

#include <string.h>

static int vj_bss_411C(spu_context* ctx)   // @0x0000411C (job_00cd7180)
{
    const uint32_t s = ctx->gpr[3]._u32[0], e = ctx->gpr[4]._u32[0];
    if (s >= e || e - s > SPU_LS_SIZE || e > 0xFFFF0000u) return 0;
    const uint32_t n = (e - s + 15) >> 4;   /* stores: the loop runs until r3 >= r4 (unsigned) */
    uint32_t a = (s + ctx->gpr[126]._u32[0]) & (SPU_LS_MASK & ~0xFu);
    uint32_t left = n * 16;
    while (left) {   /* the store address wraps at the end of the local store, as spu_ls_write128 masks it */
        const uint32_t chunk = left < SPU_LS_SIZE - a ? left : SPU_LS_SIZE - a;
        memset(ctx->ls + a, 0, chunk);
        left -= chunk;
        a = 0;
    }
    for (int k = 0; k < 4; k++) {
        ctx->gpr[3]._u32[k] += 16 * n;
        ctx->gpr[5]._u32[k] = 0;
        ctx->gpr[6]._u32[k] = ctx->gpr[4]._u32[k] > ctx->gpr[3]._u32[k] ? 0xFFFFFFFFu : 0u;
    }
    return 1;
}

/* the job's memcpy @0x7F00 (slot 78, KZ2_NATIVE_JOB_VJCOPY). Called from the main loop @0x525C and
 * @0x598C: r3 = destination, r4 = source, r5 = bytes (preferred words), returns r3 unchanged. The SPU code:
 *   n == 0: return.
 *   both 16-byte aligned (@0x7F40): whole quadwords, then the first n & 15 bytes of the next destination quadword
 *     from the source's (@0x8070: selb with rotqmby(-1, -rest), the rest of that quadword kept).
 *   else (@0x7F70): the first destination quadword's bytes [d & 15, min(16, d & 15 + n)) from the source (shifted by
 *     rotqmby when s & 15 <= d & 15, else shufb of two source quadwords with the shift table at 0x8580 + 16 k: row k
 *     = bytes k..k+15), the other bytes kept (selb masks from shlqby/rotqmby of -1); then whole destination
 *     quadwords from source quadword pairs (shufb, table row (source & 15)); the last partial one merged as above.
 * So the local store ends as memcpy(d, s, n) leaves it when the source quadwords it reads ([s & ~15, s + n + 31))
 * are not written by the copy, every address stays below the end of the local store (lqd/stqd wrap: masked
 * 0x3FFF0) and n is the preferred word (clgti: unsigned). The native code declines otherwise. Scratch registers are
 * not reproduced (r3 is). An earlier change proved that nothing reads them: tools/spu/live_proof.py walks from both call
 * sites through the rest of the job, its calls into the job manager (API 0x1820/0x1818 through the table at LS
 * 0x1330, the command dispatcher, the SPURS kernel entries) and the job manager after the job returns, with the call
 * stack above 0x50F8 seeded; none of the 54 registers the SPU memcpy writes is read before it is written again
 * (the RE notes "An earlier change").
 * The pool (the shift table rows 0-15, LS 0x8580-0x8680) is checked: the lifted code reads it. */
static int vj_memcpy_7F00(spu_context* ctx)   // @0x00007F00 (job_00cd7180)
{
    const uint32_t d = ctx->gpr[3]._u32[0], s = ctx->gpr[4]._u32[0], n = ctx->gpr[5]._u32[0];
    if (!n) return 1;
    if ((uint64_t)d + n + 16 > SPU_LS_SIZE || (uint64_t)s + n + 32 > SPU_LS_SIZE) return 0;
    const uint32_t wlo = d & ~15u, whi = (d + n + 15) & ~15u, rlo = s & ~15u, rhi = s + n + 32;
    if (wlo < rhi && rlo < whi) return 0;   /* the copy would read what it wrote */
    memcpy(ctx->ls + d, ctx->ls + s, n);
    return 1;
}

const kz2_native_entry kz2_native_particles[] = {
    { 77, "KZ2_NATIVE_JOB_VJCLEAR", "Particles.VertexJob", "job_00cd7180", 0x411Cu, vj_bss_411C, "bss_clear", 0, 0, 0,
      0x407Cu },
    { 78, "KZ2_NATIVE_JOB_VJCOPY", "Particles.VertexJob", "job_00cd7180", 0x7F00u, vj_memcpy_7F00, "vj_memcpy", 0x8580u,
      0x8680u, 0x2a2ecd46ad5ff165ull, 0 },
};
const unsigned kz2_native_particles_n = sizeof kz2_native_particles / sizeof kz2_native_particles[0];
