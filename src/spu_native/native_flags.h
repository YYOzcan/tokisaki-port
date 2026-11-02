/* From Project Pyrrhus by undecuple (https://github.com/undecuple/project-pyrrhus, port/kz2/spu/native_flags.h at 218f660),
 * MIT License, Copyright (c) 2026 undecuple -- see THIRD_PARTY_NOTICES.md. Tokisaki changes are marked "tokisaki:". */
/* native_flags.h -- which flag turns a native gate on (split out of kz2_native_jobs.c so that
 * tools/spu/test_native_flags.sh can test it with a fake environment; an earlier change: per-flag 0 wins, recorded slots).
 *
 * An entry (its slot, its `env` list of '|'-separated KZ2_NATIVE_JOB_<NAME> flags, its job) is on when, in this order:
 *   1. KZ2_NATIVE_JOB_POST=0 and the job is a Gfx.Post job: off (every Gfx.Post gate, defaults and per-job flags too);
 *   2. one of its flags is 1: on;
 *   3. one of its flags is set to anything else (0, say): off (a job's own 0 wins over KZ2_NATIVE_JOB_POST=1
 *      and over the defaults);
 *   4. KZ2_NATIVE_JOB_POST=1 and the job is a Gfx.Post job: on;
 *   5. its slot is in kz2_native_play_recorded (equality shown on inputs recorded in play) and one of its flags is in
 *      kz2_native_default_on: on;
 *   otherwise off.
 * Rule 5's slot list is the native-milestone rule's equality half: a gate whose kernel was never called in a recorded
 * run (the shared kernels' copies in the post images that write their result another way, three blur variants) is
 * not turned on by a default, only by its flag or KZ2_NATIVE_JOB_POST=1. */
#ifndef KZ2_NATIVE_FLAGS_H
#define KZ2_NATIVE_FLAGS_H

#include <stdio.h>
#include <string.h>

/* Default on: KZ2_NATIVE_JOB_DOF since an earlier change (an earlier change's A/B); the rest of Gfx.Post since an earlier change
 * (its in-play A/B, native_ab.sh, KZ2_NATIVE_JOB_POST=1 against these defaults: the RE notes "An earlier change");
 * Anim.EdgeAnim's two kernels and Particles.VertexJob's .bss clear since an earlier change (the same-run toggle A/B,
 * KZ2_NATIVE_TOGGLE, tools/spu/toggle_ab.py: the RE notes "An earlier change"); VertexJob's memcpy since an earlier change
 * (its scratch registers proven unread by tools/spu/live_proof.py, and its A/B: the RE notes "An earlier change"). */
static const char* const kz2_native_default_on[] = {
    "KZ2_NATIVE_JOB_DOF",      "KZ2_NATIVE_JOB_UPSAMPLE",     "KZ2_NATIVE_JOB_POSTMIX",   "KZ2_NATIVE_JOB_BLUR",
    "KZ2_NATIVE_JOB_BLOOM",    "KZ2_NATIVE_JOB_MODULATE",     "KZ2_NATIVE_JOB_BLOOMINT",  "KZ2_NATIVE_JOB_ILR",
    "KZ2_NATIVE_JOB_VIGNETTE", "KZ2_NATIVE_JOB_DEPTHTOFUZZY", "KZ2_NATIVE_JOB_DOWNSAMPLE", "KZ2_NATIVE_JOB_ZERO",
    "KZ2_NATIVE_JOB_DOFPASS",  "KZ2_NATIVE_JOB_EDGEANIM",     "KZ2_NATIVE_JOB_VJCLEAR",
    "KZ2_NATIVE_JOB_VJCOPY",
};

/* Slots with recorded-in-play equality (tools/spu/native_diff.py rec, bit-exact, interpreter agreeing): DOF's kernels
 * and Upsample's (Corinth River), the blur and bloom kernels called in play, and the remaining post passes; all 25
 * again on The New Sun's recording (800/800). The other 34 post slots were never called in a recorded run: off unless
 * their flag is 1 or KZ2_NATIVE_JOB_POST=1. EdgeAnim's kernels (75, 76) and VertexJob's .bss clear (77) and memcpy
 * (78) on the A/B route's recording (464/464 over 29 slots, twice). */
static const int kz2_native_play_recorded[] = { 0, 1, 2, 3, 4, 8, 23, 39, 52, 53, 55, 58, 59, 61, 64, 65,
                                                66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78 };

static inline int kz2_native_is_default_on(const char* flag)
{
    for (size_t i = 0; i < sizeof kz2_native_default_on / sizeof kz2_native_default_on[0]; i++)
        if (!strcmp(flag, kz2_native_default_on[i])) return 1;
    return 0;
}

static inline int kz2_native_is_play_recorded(int slot)
{
    for (size_t i = 0; i < sizeof kz2_native_play_recorded / sizeof kz2_native_play_recorded[0]; i++)
        if (kz2_native_play_recorded[i] == slot) return 1;
    return 0;
}

/* NULL = off; else what turned it on ("KZ2_NATIVE_JOB_X=1", "KZ2_NATIVE_JOB_POST=1", "KZ2_NATIVE_JOB_X default"),
 * written to buf. get = getenv or a test's stand-in. */
static inline const char* kz2_native_enabled_by(const char* envlist, const char* job, int slot,
                                                const char* (*get)(const char*), char* buf, size_t bufn)
{
    const int post_job = job && !strncmp(job, "Gfx.Post.", 9);
    const char* post = get("KZ2_NATIVE_JOB_POST");
    if (post_job && post && *post == '0') return NULL;
    const char* def = NULL;
    int set_other = 0;
    const char* p = envlist;
    while (p && *p) {
        const char* q = strchr(p, '|');
        const size_t n = q ? (size_t)(q - p) : strlen(p);
        char name[64];
        if (n < sizeof name) {
            memcpy(name, p, n);
            name[n] = 0;
            const char* v = get(name);
            if (v && !strcmp(v, "1")) {
                snprintf(buf, bufn, "%s=1", name);
                return buf;
            }
            if (v && *v) set_other = 1;
            else if (!def && kz2_native_is_default_on(name)) {
                snprintf(buf, bufn, "%s default", name);
                def = buf;
            }
        }
        p = q ? q + 1 : NULL;
    }
    if (set_other) return NULL;
    if (post_job && post && !strcmp(post, "1")) {
        snprintf(buf, bufn, "KZ2_NATIVE_JOB_POST=1");
        return buf;
    }
    return def && kz2_native_is_play_recorded(slot) ? def : NULL;
}

#endif
