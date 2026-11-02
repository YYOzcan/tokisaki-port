/* From Project Pyrrhus by undecuple (https://github.com/undecuple/project-pyrrhus, port/kz2/spu/native_blur.c at 218f660),
 * MIT License, Copyright (c) 2026 undecuple -- see THIRD_PARTY_NOTICES.md. Tokisaki changes are marked "tokisaki:". */
/* native_blur.c -- Gfx.Post.VBlur and Gfx.Post.HBlur as native host code.
 * Default off: KZ2_NATIVE_JOB_BLUR=1 (these twelve kernels), KZ2_NATIVE_JOB_POST=1 (every Gfx.Post gate).
 *
 * Each job's pass function (@0x4A20 in both images) calls one of six row kernels per output row, picked by the pixel
 * size (4 bytes: RGBA8; 8 bytes: four big-endian 16-bit channels) and two flag bits of its parameters. All twelve are
 * the same 7-tap binomial filter (1 6 15 20 15 6 1) / 64 along one axis, in integers (tools/spu/spu_symtrace.py on
 * each; the RE notes "An earlier change"). For output pixel x, channel c, with t_k the input pixel k steps away (k = -3..3;
 * a step is one pixel for HBlur, one row stride for VBlur), u() the channel zero-extended and s() its low 16 bits
 * signed (the SPU's mpya multiplies signed halfwords; this matters only for 16-bit channels of 0x8000 or more):
 *   var A ("plain"):   acc = u(t-3) + u(t3) + 6 s(t-2) + 15 s(t-1) + 20 s(t0) + 15 s(t1) + 6 s(t2)   (mod 2^32)
 *                      out = acc >> 6 (logical), its low byte or halfword stored
 *   var B ("bright"):  the same acc, out = min(acc >> 5, 255)  (rotmi 5, cgt 255 + selb: twice as bright, clamped)
 *   var C ("cov"):     p = u(t-3) + u(t3) + 6 s(t-2) + 6 s(t2); q = sx16(p) * (c == 0 ? 2 : 1) (mpy by the pool
 *                      constant 0x00020002 00010001 ...); acc = q + 15 s(t-1) + 20 s(t0) + 15 s(t1);
 *                      out = min(acc >> 6, 255)  (the first channel's outer taps count double)
 * The 8-byte variants clamp at 255 too (the SPU code's constant), not at 0xFFFF; the native code does the same.
 *
 * Arguments: VBlur r3 = output row, r4 = centre source row, r5 = row stride (mpyui by -3..3: only its low 16 bits),
 * r6 = pixels (4 a step; 0 never ends the SPU loop). HBlur r3 = output row, r4 = source row, r5 = groups of 20 pixels
 * (4-byte) or 10 pixels (8-byte); the row's neighbours before and after it are read from memory as it is (the bytes
 * just before and after the row). The native code wants what the game passes: 16-byte aligned rows (and stride),
 * nothing wrapping the local store, the output apart from every input byte the filter uses; otherwise the lifted code
 * runs (the SPU loops are software-pipelined, so an output over an input would depend on their store order).
 * All sums fit 16-bit lanes for byte channels (at most 78 * 255); 16-bit channels use 32-bit lanes with the mpya
 * semantics. The kernels are leaves: they keep r0, r1, r80-r127 and leave r2-r79 as they were (every call site
 * checked with tools/spu/post_kernels.py --live).
 */
#include "kz2_native_jobs.h"

#include <smmintrin.h>
#include <string.h>

enum { VAR_A, VAR_B, VAR_C };

static inline int ovl(int64_t a, int64_t an, int64_t b, int64_t bn)
{
    return a < b + bn && b < a + an;
}

/* 4-byte pixels, one 16-byte vector (4 pixels) of output from the 7 input vectors t[0..6] (k = -3..3). */
static inline __m128i blur4(const __m128i t[7], int var)
{
    const __m128i z = _mm_setzero_si128();
    const __m128i k6 = _mm_set1_epi16(6), k15 = _mm_set1_epi16(15), k20 = _mm_set1_epi16(20);
    const __m128i kc = _mm_set_epi16(1, 1, 1, 2, 1, 1, 1, 2);
    __m128i r[2];
    for (int h = 0; h < 2; h++) {
        __m128i u[7];
        for (int k = 0; k < 7; k++) u[k] = h ? _mm_unpackhi_epi8(t[k], z) : _mm_unpacklo_epi8(t[k], z);
        __m128i acc = _mm_add_epi16(_mm_add_epi16(u[0], u[6]), _mm_mullo_epi16(_mm_add_epi16(u[1], u[5]), k6));
        if (var == VAR_C) acc = _mm_mullo_epi16(acc, kc);
        acc = _mm_add_epi16(acc, _mm_mullo_epi16(_mm_add_epi16(u[2], u[4]), k15));
        acc = _mm_add_epi16(acc, _mm_mullo_epi16(u[3], k20));
        r[h] = var == VAR_B ? _mm_srli_epi16(acc, 5) : _mm_srli_epi16(acc, 6);
    }
    return _mm_packus_epi16(r[0], r[1]);   /* A: at most 255 already; B, C: the clamp at 255 */
}

/* One 8-byte pixel (four big-endian 16-bit channels) as four 32-bit lanes, zero-extended. */
static inline __m128i px8(const uint8_t* p)
{
    return _mm_shuffle_epi8(_mm_loadl_epi64((const __m128i*)p),
                            _mm_set_epi8(-1, -1, 6, 7, -1, -1, 4, 5, -1, -1, 2, 3, -1, -1, 0, 1));
}

static inline __m128i sx16(__m128i x)
{
    return _mm_srai_epi32(_mm_slli_epi32(x, 16), 16);
}

/* 8-byte pixels: one output pixel (4 channels in 32-bit lanes) from the 7 inputs, as the SPU computes it. */
static inline __m128i blur8(const __m128i t[7], int var)
{
    const __m128i k6 = _mm_set1_epi32(6), k15 = _mm_set1_epi32(15), k20 = _mm_set1_epi32(20);
    __m128i acc = _mm_add_epi32(_mm_add_epi32(t[0], t[6]),
                                _mm_add_epi32(_mm_mullo_epi32(sx16(t[1]), k6), _mm_mullo_epi32(sx16(t[5]), k6)));
    if (var == VAR_C) acc = _mm_mullo_epi32(sx16(acc), _mm_set_epi32(1, 1, 1, 2));
    acc = _mm_add_epi32(acc, _mm_add_epi32(_mm_mullo_epi32(sx16(t[2]), k15), _mm_mullo_epi32(sx16(t[4]), k15)));
    acc = _mm_add_epi32(acc, _mm_mullo_epi32(sx16(t[3]), k20));
    if (var == VAR_A) return _mm_srli_epi32(acc, 6);
    return _mm_min_epu32(var == VAR_B ? _mm_srli_epi32(acc, 5) : _mm_srli_epi32(acc, 6), _mm_set1_epi32(255));
}

/* The filter over n pixels of P bytes: input pixel k steps from output pixel x at src + x P + k step. */
static int blur(spu_context* ctx, uint32_t dst, uint32_t src, uint32_t n, uint32_t P, int64_t step, int vert, int var)
{
    uint8_t* ls = ctx->ls;
    const int64_t bytes = (int64_t)n * P;
    if (n == 0 || bytes > 0x10000 || ((dst | src) & 15)) return 0;
    if ((int64_t)dst + bytes > SPU_LS_SIZE) return 0;
    if (vert) {
        /* the rows the filter uses: src + k step, k = -3..3, each bytes long */
        if ((int64_t)src - 3 * step < 0 || (int64_t)src + 3 * step + bytes > SPU_LS_SIZE) return 0;
        for (int k = -3; k <= 3; k++)
            if (ovl(dst, bytes, (int64_t)src + k * step, bytes)) return 0;
    } else {
        const int64_t ap = 3 * (int64_t)P;
        if ((int64_t)src - ap < 0 || (int64_t)src + bytes + ap > SPU_LS_SIZE) return 0;
        if (ovl(dst, bytes, (int64_t)src - ap, bytes + 2 * ap)) return 0;
    }
    if (P == 4) {
        for (uint32_t q = 0; q < n / 4; q++) {
            const uint8_t* c = ls + src + 16 * q;
            __m128i t[7];
            for (int k = 0; k < 7; k++) t[k] = _mm_loadu_si128((const __m128i*)(c + (k - 3) * step));
            _mm_storeu_si128((__m128i*)(ls + dst + 16 * q), blur4(t, var));
        }
    } else {
        const __m128i pk = _mm_set_epi8(-1, -1, -1, -1, -1, -1, -1, -1, 12, 13, 8, 9, 4, 5, 0, 1);
        for (uint32_t x = 0; x < n; x++) {
            const uint8_t* c = ls + src + 8 * x;
            __m128i t[7];
            for (int k = 0; k < 7; k++) t[k] = px8(c + (k - 3) * step);
            _mm_storel_epi64((__m128i*)(ls + dst + 8 * x), _mm_shuffle_epi8(blur8(t, var), pk));
        }
    }
    return 1;
}

/* VBlur: r3 = output, r4 = centre row, r5 = stride (low 16 bits, a multiple of 16), r6 = pixels (a multiple of 4). */
static int vblur(spu_context* ctx, uint32_t P, int var)
{
    const uint32_t st = ctx->gpr[5]._u32[0], n = ctx->gpr[6]._u32[0];
    if (st > 0xFFFF || (st & 15) || n % 4 || n > 16384) return 0;
    return blur(ctx, ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0], n, P, st, 1, var);
}

/* HBlur: r3 = output, r4 = source row, r5 = groups (20 pixels of 4 bytes or 10 of 8: 80 bytes a group). */
static int hblur(spu_context* ctx, uint32_t P, int var)
{
    const uint32_t g = ctx->gpr[5]._u32[0];
    if (g == 0 || g > 512) return 0;
    return blur(ctx, ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0], g * (P == 4 ? 20 : 10), P, P, 0, var);
}

/* VBlur @0x57A0 / @0x59E0 / @0x5EB0 (4-byte) and @0x6170 / @0x63B0 / @0x6640 (8-byte); loops 0x58B0, 0x5D68, 0x6010,
 * 0x6240, 0x64D0, 0x67B0. */
static int v4a_57A0(spu_context* c) { return vblur(c, 4, VAR_A); }
static int v4b_59E0(spu_context* c) { return vblur(c, 4, VAR_B); }
static int v4c_5EB0(spu_context* c) { return vblur(c, 4, VAR_C); }
static int v8a_6170(spu_context* c) { return vblur(c, 8, VAR_A); }
static int v8b_63B0(spu_context* c) { return vblur(c, 8, VAR_B); }
static int v8c_6640(spu_context* c) { return vblur(c, 8, VAR_C); }
/* HBlur @0x5800 / @0x5BC8 / @0x6028 (4-byte) and @0x64F0 / @0x6778 / @0x6A30 (8-byte). */
static int h4a_5800(spu_context* c) { return hblur(c, 4, VAR_A); }
static int h4b_5BC8(spu_context* c) { return hblur(c, 4, VAR_B); }
static int h4c_6028(spu_context* c) { return hblur(c, 4, VAR_C); }
static int h8a_64F0(spu_context* c) { return hblur(c, 8, VAR_A); }
static int h8b_6778(spu_context* c) { return hblur(c, 8, VAR_B); }
static int h8c_6A30(spu_context* c) { return hblur(c, 8, VAR_C); }

/* Gates (re/spu/native_gates.tsv slots 52-63; pools from tools/spu/post_kernels.py --table). */
#define BL_ENV "KZ2_NATIVE_JOB_BLUR"
#define V "Gfx.Post.VBlur", "job_00d5ce80"
#define H "Gfx.Post.HBlur", "job_00d59d00"
const kz2_native_entry kz2_native_blur[] = {
    { 52, BL_ENV, V, 0x57A0u, v4a_57A0, "vblur4", 0x6B10u, 0x6BA0u, 0x7ebb5d25c354924full, 0 },
    { 53, BL_ENV, V, 0x59E0u, v4b_59E0, "vblur4", 0x6B10u, 0x6BA0u, 0x7ebb5d25c354924full, 0 },
    { 54, BL_ENV, V, 0x5EB0u, v4c_5EB0, "vblur4", 0x6B10u, 0x6BC0u, 0x5c4b75d3c3255097ull, 0 },
    { 55, BL_ENV, V, 0x6170u, v8a_6170, "vblur8", 0x6B70u, 0x6BB0u, 0x5f3e096af9447e1aull, 0 },
    { 56, BL_ENV, V, 0x63B0u, v8b_63B0, "vblur8", 0x6B70u, 0x6BB0u, 0x5f3e096af9447e1aull, 0 },
    { 57, BL_ENV, V, 0x6640u, v8c_6640, "vblur8", 0x6B70u, 0x6BC0u, 0x34fbd5773d38a12aull, 0 },
    { 58, BL_ENV, H, 0x5800u, h4a_5800, "hblur4", 0x6F30u, 0x6FC0u, 0x7ebb5d25c354924full, 0 },
    { 59, BL_ENV, H, 0x5BC8u, h4b_5BC8, "hblur4", 0x6F30u, 0x6FC0u, 0x7ebb5d25c354924full, 0 },
    { 60, BL_ENV, H, 0x6028u, h4c_6028, "hblur4", 0x6F30u, 0x6FE0u, 0x5c4b75d3c3255097ull, 0 },
    { 61, BL_ENV, H, 0x64F0u, h8a_64F0, "hblur8", 0x6F90u, 0x6FD0u, 0x5f3e096af9447e1aull, 0 },
    { 62, BL_ENV, H, 0x6778u, h8b_6778, "hblur8", 0x6F90u, 0x6FD0u, 0x5f3e096af9447e1aull, 0 },
    { 63, BL_ENV, H, 0x6A30u, h8c_6A30, "hblur8", 0x6F90u, 0x6FE0u, 0x34fbd5773d38a12aull, 0 },
};
const unsigned kz2_native_blur_n = sizeof kz2_native_blur / sizeof kz2_native_blur[0];
