/* From Project Pyrrhus by undecuple (https://github.com/undecuple/project-pyrrhus, port/kz2/spu/native_simd.h at 218f660),
 * MIT License, Copyright (c) 2026 undecuple -- see THIRD_PARTY_NOTICES.md. Tokisaki changes are marked "tokisaki:". */
/* native_simd.h -- what the native Gfx.Post passes share:
 * local-store access as the SPU does it, and the runtime's SPU float helpers' own SSE fast paths inline.
 *
 * Exactness rule: each wrapper computes what the helper's fast path computes (the same SSE instruction)
 * and calls the helper itself (spu_helpers.h) for any vector the helper would treat specially, so a wrapper returns
 * the helper's bits for every input:
 *   fm_/fa_/fs_   mulps/addps/subps; a result lane with biased exponent 255 sends the vector to spu_fm/fa/fs (KZ3
 *                 patch 1105: an infinity or NaN operand always gives an exponent-255 result, so the result's mask is
 *                 the helper's own test)
 *   fma_/fnms_    spu__fma4's -mfma path (KZ3 1097/1106): one fused multiply-add when every result lane is normal or
 *                 zero and the slow-cg mode is off; otherwise spu_fma/spu_fnms
 *   fcgt_         cmpgtps unless an operand lane has exponent 255 (spu_fcgt)
 *   cfltu_        x 2^(173-i8) truncated, for i8 <= 173 (a power-of-two scale >= 1: the float product is exact); lanes
 *                 with exponent 255 or a product at or above 2^31 send the vector to spu_cfltu
 * The helpers are lane-wise (each result lane depends only on the same lane of the operands), so the native passes
 * may put any pixel's value in any lane.
 */
#ifndef KZ2_NATIVE_SIMD_H
#define KZ2_NATIVE_SIMD_H

#include "kz2_native_jobs.h"
#include "spu_helpers.h"

#include <smmintrin.h>
#include <string.h>

#define LSM(a) ((a) & SPU_LS_MASK)
#define LSQ(a) ((a) & (SPU_LS_MASK & ~0xFu))

/* the quadword at LS a (masked as lqd/lqx do) as the runtime's u128 (_u32[0] = SPU word 0), and back */
static inline u128 ldq(const uint8_t* ls, uint32_t a)
{
    const __m128i bs = _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
    __m128i v = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + LSQ(a))), bs);
    u128 r;
    memcpy(&r, &v, 16);
    return r;
}

static inline void stq(uint8_t* ls, uint32_t a, u128 x)
{
    const __m128i bs = _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
    __m128i v;
    memcpy(&v, &x, 16);
    _mm_storeu_si128((__m128i*)(ls + LSQ(a)), _mm_shuffle_epi8(v, bs));
}

static inline uint32_t be32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* The SPU word at byte address a as `lqd/lqx` + `rotqby` by a read it: bytes a, a+1, a+2, a+3 of the quadword at
 * a & ~15, wrapping inside that quadword. */
static inline uint32_t rdw(const uint8_t* ls, uint32_t a)
{
    const uint8_t* q = ls + LSQ(a);
    return (uint32_t)q[a & 15] << 24 | (uint32_t)q[(a + 1) & 15] << 16 | (uint32_t)q[(a + 2) & 15] << 8 |
           q[(a + 3) & 15];
}

static inline u128 sp(uint32_t v)
{
    return spu_splat_u32(v);
}

static inline __m128 F(u128 x) { __m128 r; memcpy(&r, &x, 16); return r; }
static inline u128 U(__m128 x) { u128 r; memcpy(&r, &x, 16); return r; }
static inline __m128 fbits(uint32_t v) { return _mm_castsi128_ps(_mm_set1_epi32((int)v)); }

static inline int e255(__m128 r)   /* a lane with biased exponent 255 */
{
    const __m128i e = _mm_set1_epi32(0x7F800000);
    return _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(r), e), e)));
}
static inline __m128 fm_(__m128 a, __m128 b)   /* spu_fm: mulps, exponent-255 result lanes recomputed */
{
    const __m128 r = _mm_mul_ps(a, b);
    return __builtin_expect(e255(r), 0) ? F(spu_fm(U(a), U(b))) : r;
}
static inline __m128 fa_(__m128 a, __m128 b)
{
    const __m128 r = _mm_add_ps(a, b);
    return __builtin_expect(e255(r), 0) ? F(spu_fa(U(a), U(b))) : r;
}
static inline __m128 fs_(__m128 a, __m128 b)
{
    const __m128 r = _mm_sub_ps(a, b);
    return __builtin_expect(e255(r), 0) ? F(spu_fs(U(a), U(b))) : r;
}
static inline int fma_ok(__m128 r)   /* spu__fma4's test (KZ3 1106): every lane normal or zero */
{
    const __m128i ab = _mm_and_si128(_mm_castps_si128(r), _mm_set1_epi32(0x7FFFFFFF));
    const __m128i nn = _mm_cmpgt_epi32(_mm_add_epi32(ab, _mm_set1_epi32(0x7F000000)), _mm_set1_epi32(-0x01800001));
    return _mm_testz_si128(nn, ab);
}
static inline __m128 fma_(__m128 a, __m128 b, __m128 c)   /* spu_fma = a b + c */
{
#if defined(__FMA__)
    if (__builtin_expect(g_kz3_spu_slowcg == 0, 1)) {
        const __m128 r = _mm_fmadd_ps(a, b, c);
        if (__builtin_expect(fma_ok(r), 1)) return r;
    }
#endif
    return F(spu_fma(U(a), U(b), U(c)));
}
static inline __m128 fnms_(__m128 a, __m128 b, __m128 c)   /* spu_fnms = c - a b (b negated, as spu__fma4 does) */
{
#if defined(__FMA__)
    if (__builtin_expect(g_kz3_spu_slowcg == 0, 1)) {
        const __m128 r = _mm_fmadd_ps(a, _mm_xor_ps(b, _mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u))), c);
        if (__builtin_expect(fma_ok(r), 1)) return r;
    }
#endif
    return F(spu_fnms(U(a), U(b), U(c)));
}
static inline __m128 fcgt_(__m128 a, __m128 b)   /* spu_fcgt: cmpgtps unless an operand lane has exponent 255 */
{
    if (__builtin_expect(e255(a) | e255(b), 0)) return F(spu_fcgt(U(a), U(b)));
    return _mm_cmpgt_ps(a, b);
}
/* x > hi ? hi : x, then x > 0 ? x : 0 (fcgt + selb twice, the SPU code's clamp) */
static inline __m128 clamp_(__m128 x, __m128 hi)
{
    x = _mm_blendv_ps(x, hi, fcgt_(x, hi));
    const __m128 z = _mm_setzero_ps();
    return _mm_blendv_ps(z, x, fcgt_(x, z));
}
/* spu_cfltu(x, i8), i8 <= 173: exact when every lane is below 2^31 after the scale and has no exponent 255 (the helper
 * floors max(0, x 2^s) in double; the float product is exact, negative lanes give 0). */
static inline __m128i cfltu_(__m128 x, int i8)
{
    const __m128 p = _mm_mul_ps(x, _mm_set1_ps((float)(1u << (173 - i8))));
    if (__builtin_expect(e255(x) | _mm_movemask_ps(_mm_cmpge_ps(p, _mm_set1_ps(2147483648.0f))), 0)) {
        const u128 r = spu_cfltu(U(x), i8);
        __m128i v;
        memcpy(&v, &r, 16);
        return v;
    }
    return _mm_cvttps_epi32(_mm_max_ps(p, _mm_setzero_ps()));
}

static inline int ovl(int64_t a, int64_t an, int64_t b, int64_t bn)
{
    return an > 0 && bn > 0 && a < b + bn && b < a + an;
}

/* SPU word lanes (big-endian in the local store) <-> host lanes */
static inline __m128i bswap32x4(__m128i v)
{
    return _mm_shuffle_epi8(v, _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3));
}

#endif
