/* From Project Pyrrhus by undecuple (https://github.com/undecuple/project-pyrrhus, port/kz2/spu/kz2_native_jobs.h at 218f660),
 * MIT License, Copyright (c) 2026 undecuple -- see THIRD_PARTY_NOTICES.md. Tokisaki changes are marked "tokisaki:". */
/* kz2_native_jobs.h -- native host versions of hot SPU job code.
 *
 * A native gate is a check at the top of a lifted SPU function (inserted by tools/spu/lift_spu.py from
 * re/spu/native_gates.tsv): when g_kz2_native_gate[slot] is set and returns 1, the native code has done the whole
 * subroutine (its local-store writes and the registers its caller reads) and the lifted function returns to its link
 * as the SPU's `bi $r0` would; a "continue at" gate (native_gates.tsv column 7, the entry's `cont`) covers
 * code that is not a subroutine and hops on to that address instead. Returning 0 runs the lifted body (unexpected arguments, a debug gate armed, the job's
 * constant tables not what the native code was written for). Which gates are filled: native_flags.h. A gate is
 * filled when one of its entry's flags is 1 (`env` lists KZ2_NATIVE_JOB_<NAME> alternatives separated by '|'), or
 * KZ2_NATIVE_JOB_POST=1 for a Gfx.Post gate, or by default: every Gfx.Post flag is on by default
 * for the 25 slots with equality on inputs recorded in play (KZ2_NATIVE_JOB_DOF only). A flag set to 0
 * turns its entries off; KZ2_NATIVE_JOB_POST=0 turns every Gfx.Post gate off. Non-post jobs (Anim.EdgeAnim, round
 * 19: KZ2_NATIVE_JOB_EDGEANIM) stay off unless their flag is 1.
 *
 * Every gate runs through kz2_native_run (native_common.c): it counts the call, declines while a debug LS gate is
 * armed, checks the entry's constant pool (the LS range its kernel's lqr loads read, against the hash of the 1.29
 * image's bytes: a different image or rewritten constants run the lifted code) and then calls the kernel, which
 * checks its own arguments. A kernel returns 1 when it did the whole subroutine, 0 to run the lifted body.
 *
 * KZ2_NATIVE_REC=DIR records real inputs instead (every gate, flags or not): the whole context (128 registers, the
 * 256 KB local store) at the gate, written to DIR/s<slot>_<n>.kz2nj, then the lifted body runs. KZ2_NATIVE_REC_N
 * (default 24) records per slot, KZ2_NATIVE_REC_EVERY (default 61) calls between records, KZ2_NATIVE_REC_AFTER
 * (default 120) seconds before the first; KZ2_NATIVE_REC_DOF_BLUR=1 counts only DepthOfField gather calls with a blur
 * radius above 0. Recording also counts the calls per slot (`[native] slot N ... calls=`). The records
 * hold game data: keep them under work/, never commit them.
 * tools/spu/native_diff.py replays them through the lifted and the native code and compares (native_diff_harness.c).
 */
#ifndef KZ2_NATIVE_JOBS_H
#define KZ2_NATIVE_JOBS_H

#include <stdint.h>
#include "spu_context.h"
#include "kz2_native_port.h"   /* tokisaki: what this runtime lacks (see there) */

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*kz2_native_fn)(spu_context*);

typedef struct {
    int slot;              /* g_kz2_native_gate index (re/spu/native_gates.tsv) */
    const char* env;       /* KZ2_NATIVE_JOB_<NAME>, alternatives separated by '|' */
    const char* job;       /* census name */
    const char* image;     /* lifted image (job_<file offset>), as in native_gates.tsv */
    uint32_t addr;         /* LS address of the gated function */
    kz2_native_fn fn;
    const char* kind;      /* kernel family (the diff harness picks its synthetic inputs by it) */
    uint32_t pool_lo, pool_hi;   /* constant pool the kernel was written for: LS [lo, hi), 8-byte aligned */
    uint64_t pool_hash;          /* FNV-1a 64 over its little-endian 8-byte words (tools/spu/post_kernels.py) */
    uint32_t cont;         /* 0: a subroutine (the gate returns to r0); else a "continue at" gate: the
                            * native code did the code from addr up to this LS address, and the gate line hops there
                            * (native_gates.tsv column 7 names the same address; tools/spu/test_native_vjclear.sh
                            * checks the lifted gate line) */
} kz2_native_entry;

extern const kz2_native_entry kz2_native_dof[];
extern const unsigned kz2_native_dof_n;
extern const kz2_native_entry kz2_native_post[];
extern const unsigned kz2_native_post_n;
extern const kz2_native_entry kz2_native_blur[];
extern const unsigned kz2_native_blur_n;
extern const kz2_native_entry kz2_native_bloom[];
extern const unsigned kz2_native_bloom_n;
extern const kz2_native_entry kz2_native_postjobs[];   /* native_postjobs.c */
extern const unsigned kz2_native_postjobs_n;
extern const kz2_native_entry kz2_native_edgeanim[];   /* native_edgeanim.c */
extern const unsigned kz2_native_edgeanim_n;
extern const kz2_native_entry kz2_native_particles[];  /* native_particles.c */
extern const unsigned kz2_native_particles_n;

/* native_common.c: the entry of a slot (NULL: none), and one gated call (count, debug gate, pool, kernel; counts a
 * fallback when it returns 0). */
const kz2_native_entry* kz2_native_find(int slot);
int kz2_native_run(spu_context* ctx, const kz2_native_entry* e);

/* Record file layout (little-endian host): magic "KZ2NJ001", u32 slot, u32 addr, s32 image_id, u32 resident_ovl,
 * then 128 x 16 bytes of registers (the runtime's u128 layout, _u32[0] = SPU word 0), then the 256 KB local store. */
#define KZ2_NJ_MAGIC "KZ2NJ001"

/* Gate slots (128): g_kz2_native_gate[] in the generated spu_workloads.c has this size. */
#define KZ2_NATIVE_SLOTS 128

/* Calls and fallbacks per slot (the native function counts its own fallbacks). kz2_native_count prints
 * `[native] slot N calls=C fallback=F` at 1024 calls and every power of two after. */
extern volatile long g_kz2_native_calls[KZ2_NATIVE_SLOTS], g_kz2_native_fallback[KZ2_NATIVE_SLOTS];
void kz2_native_count_print(int slot, long calls);
/* tokisaki: counted per thread and added to the shared count 1,024 at a time -- the shared counters sit in a few
 * cache lines, and with the lanes calling native slots ~10^5 times a frame every call's atomic add moved one of
 * them between cores. The print still fires as the shared count passes 1024 and every power of two after. */
static inline void kz2_native_count(int slot)
{
    static __thread unsigned t_pending[KZ2_NATIVE_SLOTS];
    if (__builtin_expect(++t_pending[slot] < 1024u, 1)) return;
    t_pending[slot] = 0;
    const long c = __atomic_add_fetch(&g_kz2_native_calls[slot], 1024, __ATOMIC_RELAXED);
    if (__builtin_expect(c >= 1024 && !(c & (c - 1)), 0)) kz2_native_count_print(slot, c);
}

#ifdef __cplusplus
}
#endif
#endif
