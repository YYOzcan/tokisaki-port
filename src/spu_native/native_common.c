/* From Project Pyrrhus by undecuple (https://github.com/undecuple/project-pyrrhus, port/kz2/spu/native_common.c at 218f660),
 * MIT License, Copyright (c) 2026 undecuple -- see THIRD_PARTY_NOTICES.md. Tokisaki changes are marked "tokisaki:". */
/* native_common.c -- what every native SPU gate shares: the slot registry over the per-job tables,
 * the constant-pool check and the gated call. Linked into the game (with kz2_native_jobs.c) and into the diff
 * harness (tools/spu/native_diff_harness.c), so the harness tests the same path the game runs. */
#include "kz2_native_jobs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const kz2_native_entry* kz2_native_find(int slot)
{
    for (unsigned i = 0; i < kz2_native_dof_n; i++)
        if (kz2_native_dof[i].slot == slot) return &kz2_native_dof[i];
    for (unsigned i = 0; i < kz2_native_post_n; i++)
        if (kz2_native_post[i].slot == slot) return &kz2_native_post[i];
    for (unsigned i = 0; i < kz2_native_blur_n; i++)
        if (kz2_native_blur[i].slot == slot) return &kz2_native_blur[i];
    for (unsigned i = 0; i < kz2_native_bloom_n; i++)
        if (kz2_native_bloom[i].slot == slot) return &kz2_native_bloom[i];
    for (unsigned i = 0; i < kz2_native_postjobs_n; i++)
        if (kz2_native_postjobs[i].slot == slot) return &kz2_native_postjobs[i];
    for (unsigned i = 0; i < kz2_native_edgeanim_n; i++)
        if (kz2_native_edgeanim[i].slot == slot) return &kz2_native_edgeanim[i];
    for (unsigned i = 0; i < kz2_native_particles_n; i++)
        if (kz2_native_particles[i].slot == slot) return &kz2_native_particles[i];
    return NULL;
}

static uint64_t pool_hash(const uint8_t* ls, uint32_t lo, uint32_t hi)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t a = lo; a < hi; a += 8) {
        uint64_t v;
        memcpy(&v, ls + a, 8);
        h = (h ^ v) * 0x100000001b3ull;
    }
    return h;
}

/* The hash costs ~0.1-0.2 us, as much as a small kernel call: it is checked again only when the job header at LS
 * 0x4000 (the module's 16-byte signature, its entry and size words; the 36 signatures are unique) differs from the
 * one last verified for this slot on that local store. The jobs never write their own constant pools. A cache line
 * is published as: ls = NULL, header, ls (release); a reader that sees its ls compares the header and then checks
 * that the line still holds its ls. */
#define POOL_WAYS 8
static struct {
    const uint8_t* volatile ls;
    uint8_t hdr[48];
} s_pool[KZ2_NATIVE_SLOTS][POOL_WAYS];
static volatile long s_pool_lock;

static int pool_ok(const uint8_t* ls, const kz2_native_entry* e)
{
    if (e->pool_hi <= e->pool_lo) return 1;
    const int s = e->slot & (KZ2_NATIVE_SLOTS - 1);
    for (int i = 0; i < POOL_WAYS; i++)
        if (__atomic_load_n(&s_pool[s][i].ls, __ATOMIC_ACQUIRE) == ls) {
            const int same = !memcmp(s_pool[s][i].hdr, ls + 0x4000, 48);
            if (same && __atomic_load_n(&s_pool[s][i].ls, __ATOMIC_ACQUIRE) == ls) return 1;
            break;
        }
    if (e->pool_hi > SPU_LS_SIZE || pool_hash(ls, e->pool_lo, e->pool_hi) != e->pool_hash) return 0;
    while (__sync_lock_test_and_set(&s_pool_lock, 1)) { }
    int k = -1;
    for (int i = 0; i < POOL_WAYS && k < 0; i++)
        if (s_pool[s][i].ls == ls) k = i;
    for (int i = 0; i < POOL_WAYS && k < 0; i++)
        if (!s_pool[s][i].ls) k = i;
    if (k < 0) k = (int)((uintptr_t)ls >> 12) % POOL_WAYS;
    __atomic_store_n(&s_pool[s][k].ls, (const uint8_t*)0, __ATOMIC_RELEASE);
    memcpy(s_pool[s][k].hdr, ls + 0x4000, 48);
    __atomic_store_n(&s_pool[s][k].ls, ls, __ATOMIC_RELEASE);
    __sync_lock_release(&s_pool_lock);
    return 1;
}

/* tokisaki: KZ2_NATIVE_CHECK=<slot>[,<slot>...] (unset: off) -- each call of those slots (subroutine gates) also
 * runs the lifted function, on a copy of the context and its local store, before the native one runs on the real
 * ones; afterwards the two local stores and r0, r1, r80-r127 (what the kernels keep) are compared. A difference is
 * logged and the lifted result is the one kept. KZ2_NATIVE_CHECK_EVERY=N (default 1): every Nth call. For the slots
 * no recorded run verified (native_flags.h rule 5); a checked call copies 256 KB. */
typedef void (*kz2_lifted_fn)(spu_context*);
extern kz2_lifted_fn spu_lookup(uint32_t addr, int image_id);
static _Thread_local int t_in_check;
static unsigned char s_check_slot[KZ2_NATIVE_SLOTS];
static long s_check_every = 1;
static volatile long s_checked[KZ2_NATIVE_SLOTS], s_differ[KZ2_NATIVE_SLOTS], s_check_seen[KZ2_NATIVE_SLOTS];

static int check_wanted(int slot)
{
    static int init;
    if (!__atomic_load_n(&init, __ATOMIC_ACQUIRE)) {
        static volatile long lock;
        while (__sync_lock_test_and_set(&lock, 1)) { }
        if (!init) {
            const char* e = getenv("KZ2_NATIVE_CHECK");
            for (const char* p = e; p && *p;) {
                char* end;
                const long v = strtol(p, &end, 0);
                if (end == p) break;
                if (v >= 0 && v < KZ2_NATIVE_SLOTS) s_check_slot[v] = 1;
                p = *end ? end + 1 : end;
            }
            const char* n = getenv("KZ2_NATIVE_CHECK_EVERY");
            if (n && atol(n) > 1) s_check_every = atol(n);
            if (e && *e) fprintf(stderr, "[native-check] checking slots %s (every %ld calls)\n", e, s_check_every);
            __atomic_store_n(&init, 1, __ATOMIC_RELEASE);
        }
        __sync_lock_release(&lock);
    }
    if (!s_check_slot[slot]) return 0;
    return __atomic_fetch_add(&s_check_seen[slot], 1, __ATOMIC_RELAXED) % s_check_every == 0;
}

/* 1: done (the native result, or the lifted one where they differ); 0: the native kernel declined. */
static int check_run(spu_context* ctx, const kz2_native_entry* e)
{
    static _Thread_local uint8_t* ref_ls;
    static _Thread_local spu_context* ref;
    kz2_lifted_fn lifted = ctx->resident_ovl ? spu_lookup(e->addr, ctx->resident_ovl) : NULL;
    if (!lifted) lifted = spu_lookup(e->addr, ctx->image_id);
    if (!ref_ls) ref_ls = (uint8_t*)aligned_alloc(128, SPU_LS_SIZE);
    if (!ref) ref = (spu_context*)aligned_alloc(64, (sizeof(spu_context) + 63) / 64 * 64);
    if (!lifted || !ref_ls || !ref) return e->fn(ctx);
    memcpy(ref_ls, ctx->ls, SPU_LS_SIZE);
    *ref = *ctx;
    ref->ls = ref_ls;
    ref->host_depth = ctx->host_depth + 1;   /* its SPU_RET returns here */
    t_in_check = 1;
    lifted(ref);
    t_in_check = 0;
    if (!e->fn(ctx)) return 0;
    const long n = __atomic_add_fetch(&s_checked[e->slot], 1, __ATOMIC_RELAXED);
    int bad = memcmp(ctx->gpr, ref->gpr, 2 * sizeof ctx->gpr[0]) ||
              memcmp(&ctx->gpr[80], &ref->gpr[80], 48 * sizeof ctx->gpr[0]);
    uint32_t first = 0, nbytes = 0;
    if (memcmp(ctx->ls, ref_ls, SPU_LS_SIZE)) {
        bad = 1;
        for (uint32_t a = 0; a < SPU_LS_SIZE; a++)
            if (ctx->ls[a] != ref_ls[a]) { if (!nbytes++) first = a; }
    }
    if (bad) {
        const long d = __atomic_add_fetch(&s_differ[e->slot], 1, __ATOMIC_RELAXED);
        if (d <= 8 || !(d & (d - 1)))
            fprintf(stderr, "[native-check] slot %d %s 0x%05X: DIFFERS (%ld of %ld checked): %u LS bytes from 0x%05X "
                            "(native %02X lifted %02X), r3=%05X r4=%05X r5=%X r6=%X\n",
                    e->slot, e->job, e->addr, d, n, nbytes, first, nbytes ? ctx->ls[first] : 0,
                    nbytes ? ref_ls[first] : 0, ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0], ctx->gpr[5]._u32[0],
                    ctx->gpr[6]._u32[0]);
        memcpy(ctx->ls, ref_ls, SPU_LS_SIZE);
        memcpy(ctx->gpr, ref->gpr, sizeof ctx->gpr);
    } else if (n >= 256 && !(n & (n - 1))) {
        fprintf(stderr, "[native-check] slot %d %s 0x%05X: %ld checked, %ld differ\n", e->slot, e->job, e->addr, n,
                s_differ[e->slot]);
    }
    return 1;
}

int kz2_native_run(spu_context* ctx, const kz2_native_entry* e)
{
    if (t_in_check) return 0;   /* tokisaki: check_run's reference call runs the lifted body */
    kz2_native_count(e->slot);
    if (!spu_ls_dbg_armed() && pool_ok(ctx->ls, e) && !e->cont && check_wanted(e->slot)) {   /* tokisaki */
        if (check_run(ctx, e)) return 1;
    } else if (!spu_ls_dbg_armed() && pool_ok(ctx->ls, e) && e->fn(ctx)) return 1;
    __atomic_fetch_add(&g_kz2_native_fallback[e->slot], 1, __ATOMIC_RELAXED);
    return 0;
}
