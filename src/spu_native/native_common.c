/* From Project Pyrrhus by undecuple (https://github.com/undecuple/project-pyrrhus, port/kz2/spu/native_common.c at 218f660),
 * MIT License, Copyright (c) 2026 undecuple -- see THIRD_PARTY_NOTICES.md. Tokisaki changes are marked "tokisaki:". */
/* native_common.c -- what every native SPU gate shares: the slot registry over the per-job tables,
 * the constant-pool check and the gated call. Linked into the game (with kz2_native_jobs.c) and into the diff
 * harness (tools/spu/native_diff_harness.c), so the harness tests the same path the game runs. */
#include "kz2_native_jobs.h"

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

int kz2_native_run(spu_context* ctx, const kz2_native_entry* e)
{
    kz2_native_count(e->slot);
    if (!spu_ls_dbg_armed() && pool_ok(ctx->ls, e) && e->fn(ctx)) return 1;
    __atomic_fetch_add(&g_kz2_native_fallback[e->slot], 1, __ATOMIC_RELAXED);
    return 0;
}
