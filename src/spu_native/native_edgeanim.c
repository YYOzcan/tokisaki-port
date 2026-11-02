/* From Project Pyrrhus by undecuple (https://github.com/undecuple/project-pyrrhus, port/kz2/spu/native_edgeanim.c at 218f660),
 * MIT License, Copyright (c) 2026 undecuple -- see THIRD_PARTY_NOTICES.md. Tokisaki changes are marked "tokisaki:". */
/* native_edgeanim.c -- Anim.EdgeAnim (job_00d95d80, census image 224) kernels as native host code. Default off:
 * KZ2_NATIVE_JOB_EDGEANIM=1 turns on both kernels, KZ2_NATIVE_JOB_EDGEANIM_KEYS=1 the sampler alone (not a Gfx.Post
 * job: KZ2_NATIVE_JOB_POST does not reach it).
 *
 * Profile (kz2-spu19-scout2, Corinth Sands in play, KZ3_PROF 95-165 s, the RE notes "An earlier change"): the job is
 * 6.7 SPU-ms per frame there; 29 % of its host samples are two leaf subroutines that the joint-pose decoder @0x10D78
 * calls three times each (rotation, translation, scale; no calls, no channel access, no DMA):
 *   0x13100 (521 instructions, loop 0x13428-0x1384C, 15 % of the job's samples): the bit-packed decoder, native here;
 *   0x12438 (818 instructions, loop 0x125A8-0x13034, 14 %): the keyframe sampler, native here.
 * Gated at their entries (re/spu/native_gates.tsv slots 75, 76). Written from the disassembly and
 * tools/spu/spu_symtrace.py's dataflow (synthetic arguments and states recorded in play). */
#include "native_simd.h"

/* ---- 0x13100: the bit-packed three-component decoder ----
 *
 * Arguments (preferred word of each): r3 = byte address of the bit stream, r4 = element count n, r5 = output base,
 * r6 = index table (one big-endian halfword per element), r7 = the 32-bit packing spec (any byte address). The spec
 * describes three components c = 1, 2, 3, each as s (1 bit), e (4 bits), m (5 bits): from bit 31 down, (s1, e1, m1),
 * (s2, e2, m2), (s3, e3, m3), then two bits p. An element is T = sum(s + e + m) bits, element k at bit
 * 8 r3 + k T (big-endian bit order). Its components are the top 32 bits of the stream at the element's bit, at
 * + T1 and at + T1 + T2 (Tc = sc + ec + mc), each decoded by ea_comp below. Then
 *   w = sqrt(max(0, 1 - c1^2 - c2^2 - c3^2)) (frsqest + fi + one Newton step, times the argument)
 * and the quadword (c1, c2, c3 with w inserted at index p) is stored at r5 + 48 idx[k]: a joint's 48-byte
 * (rotation, translation, scale) record; the caller @0x10D78 runs it with r5 + 0, + 16, + 32. The w of a
 * translation or a scale is computed the same way (the caller's spec puts it where it is not read).
 * The loop runs ceil(n / 4) times, four elements each (elements n .. 4 ceil(n / 4) - 1 too, with their index table
 * entries), storing elements 0, 1, 3, 2 of each group in that order. It saves r80-r127 at r1 - 16 (k - 79) and
 * restores them; no other local store write. Bit operations follow the SPU's rules (rot by count & 31; rotm, shl
 * and rotma with counts of 32 and more give 0 or the sign). Float work is the runtime's own helpers, lane-wise: the
 * four elements of a group in the four lanes. Declines when an output quadword overlaps what the kernel reads (the
 * stream, the index table, the spec) or the stack save area. */
static inline uint32_t rotl32(uint32_t x, uint32_t c) { c &= 31; return c ? (x << c) | (x >> (32 - c)) : x; }
static inline uint32_t shl_(uint32_t x, uint32_t c) { c &= 63; return c > 31 ? 0 : x << c; }
static inline uint32_t rotm_(uint32_t x, uint32_t c) { c = (0u - c) & 63; return c > 31 ? 0 : x >> c; }
static inline uint32_t rotma_(uint32_t x, uint32_t c) { c = (0u - c) & 63; return (uint32_t)((int32_t)x >> (c > 31 ? 31 : c)); }

/* the 128 bits of the stream from bit B (big-endian), as two lqd at (B >> 3) and (B >> 3) + 16 and the rotates */
static inline unsigned __int128 ea_q(const uint8_t* q)   /* a quadword as a big-endian 128-bit number */
{
    uint64_t h, l;
    memcpy(&h, q, 8);
    memcpy(&l, q + 8, 8);
    return (unsigned __int128)__builtin_bswap64(h) << 64 | __builtin_bswap64(l);
}
static inline unsigned __int128 ea_window(const uint8_t* ls, uint32_t B)
{
    const uint32_t a = B >> 3, s = B & 127;
    const unsigned __int128 A = ea_q(ls + LSQ(a));
    return s ? (A << s) | (ea_q(ls + LSQ(a + 16)) >> (128 - s)) : A;
}
/* top 32 bits after shlqbybi + shlqbi by c (bytes (c >> 3) & 31, 16 or more: 0; bits c & 7) */
static inline unsigned __int128 shlq_(unsigned __int128 x, uint32_t c)
{
    const uint32_t by = (c >> 3) & 31, bi = c & 7;
    if (by >= 16) return 0;
    x <<= 8 * by;
    return x << bi;
}

/* index k of group g: lqd + rotqby at r6 + 8 g read 8 bytes wrapping inside that quadword */
static inline uint32_t ea_idx(const uint8_t* ls, uint32_t r6, uint32_t g, uint32_t k)
{
    const uint32_t a = r6 + 8 * g;
    const uint8_t* q = ls + LSQ(a);
    return (uint32_t)q[(a + 2 * k) & 15] << 8 | q[(a + 2 * k + 1) & 15];
}

/* spu_csflt(v, 155) (x 2^0): the helper converts through double and rounds once, so a lane below 2^24 in
 * magnitude (INT_MIN too: a power of two) is cvtdq2ps exactly; any other vector goes to the helper */
static inline __m128 csflt0_(u128 v)
{
    __m128i A;
    memcpy(&A, &v, 16);
    if (__builtin_expect(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(_mm_abs_epi32(A), _mm_set1_epi32(1 << 24)))), 0))
        return F(spu_csflt(v, 155));
    return _mm_cvtepi32_ps(A);
}

/* spu_cuflt(v, 155): a lane below 2^24 converts exactly (cvtdq2ps); any other vector goes to the helper */
static inline __m128 cuflt0_(u128 v)
{
    __m128i A;
    memcpy(&A, &v, 16);
    if (__builtin_expect(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(_mm_setzero_si128(), _mm_sub_epi32(_mm_set1_epi32(1 << 24), A)))) |
                         _mm_movemask_ps(_mm_castsi128_ps(A)), 0))
        return F(spu_cuflt(v, 155));
    return _mm_cvtepi32_ps(A);
}

typedef struct { uint32_t s, e, m; } ea_field;

/* One component from the top 32 bits w: e > 0 a small float (sign s bits, e exponent bits biased by
 * 2^(e - 1) - 1 against 127, m mantissa bits at the top of the mantissa: m > 23 drops them, as shl does); e = 0 a
 * signed fixed-point value (m bits plus the sign) times 1 / (2^m - 1) (frest, fi, one Newton step; 0 when m = 0).
 * The float case is the bits themselves (int_out, is_int = 1), the fixed case the integer for csflt and its divisor
 * for cuflt (computed by the caller in lanes). */
static inline void ea_comp(uint32_t w, ea_field f, uint32_t* bits, uint32_t* ival, uint32_t* dval)
{
    const uint32_t r100 = rotl32(w, f.s), r101 = rotl32(r100, f.e);
    const uint32_t E = r101 & rotm_(0xFFFFFFFFu, f.e - 32);
    const uint32_t ex = shl_(E + (128u - shl_(1u, f.e - 1)), 23);
    const uint32_t M = rotl32(r101, f.m) & rotm_(0xFFFFFFFFu, f.m - 32);
    const uint32_t S = shl_(r100 & rotm_(0xFFFFFFFFu, f.s - 32), 31);
    *bits = ex | shl_(M, 23 - f.m) | S;
    *ival = M | rotma_(S, f.m - 31);
    *dval = rotm_(0x7FFFFFFFu, f.m - 31);
}

static int ea_13100(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t n = ctx->gpr[4]._u32[0];
    if (n == 0) return 1;   /* brz $r4: straight to the return, nothing written */
    const uint32_t r1 = ctx->gpr[1]._u32[0], r3 = ctx->gpr[3]._u32[0], r5 = ctx->gpr[5]._u32[0];
    const uint32_t r6 = ctx->gpr[6]._u32[0], r7 = ctx->gpr[7]._u32[0];
    const uint32_t spec = (uint32_t)ls[LSM(r7)] << 24 | (uint32_t)ls[LSM(r7 + 1)] << 16 |
                          (uint32_t)ls[LSM(r7 + 2)] << 8 | ls[LSM(r7 + 3)];
    const ea_field f1 = { (spec >> 31) & 1, (spec >> 27) & 15, (spec >> 22) & 31 };
    const ea_field f2 = { (spec >> 21) & 1, (spec >> 17) & 15, (spec >> 12) & 31 };
    const ea_field f3 = { (spec >> 11) & 1, (spec >> 7) & 15, (spec >> 2) & 31 };
    const uint32_t T1 = f1.s + f1.e + f1.m, T2 = f2.s + f2.e + f2.m, T = T1 + T2 + f3.s + f3.e + f3.m;
    const uint32_t iters = (n + 3) >> 2, bit0 = r3 << 3;
    /* what it reads: the stream bytes, the index table (8 bytes a group, quadword-wrapped), the spec, the stack area */
    const int64_t s_lo = (int64_t)LSQ(bit0 >> 3), s_n = (int64_t)(((uint64_t)T * 4 * iters + 7) >> 3) + 48;
    if ((uint64_t)T * 4 * iters >= ((uint64_t)SPU_LS_SIZE << 3) || s_lo + s_n > SPU_LS_SIZE) return 0;
    if (LSQ(r6) + 8u * iters + 16 > SPU_LS_SIZE || LSQ(r1 - 0x300u) + 0x300 > SPU_LS_SIZE) return 0;
    for (uint32_t g = 0; g < iters; g++)
        for (uint32_t k = 0; k < 4; k++) {
            const uint32_t idx = ea_idx(ls, r6, g, k);
            const int64_t o = (int64_t)LSQ(r5 + 48 * idx);
            if (ovl(o, 16, s_lo, s_n) || ovl(o, 16, LSQ(r6), 8 * iters + 16) || ovl(o, 16, LSQ(r7), 32) ||
                ovl(o, 16, LSQ(r1 - 0x300u), 0x300))
                return 0;
        }
    for (int k = 80; k <= 127; k++) stq(ls, r1 - 16 * (uint32_t)(k - 79), ctx->gpr[k]);
    const uint32_t kk = ~spec & 3;   /* w goes to lane p = spec & 3 */
    const __m128 one = fbits(0x3F800000u), half = fbits(0x3F000000u), zero = _mm_setzero_ps();
    /* the fixed-point components' 1 / (2^m - 1): the same in every lane and group (cuflt, frest, fi, one Newton step;
     * 0 when the divisor is 0), so computed once */
    const ea_field fs[3] = { f1, f2, f3 };
    __m128 rcp[3];
    for (int c = 0; c < 3; c++) {
        const u128 d = spu_cuflt(sp(rotm_(0x7FFFFFFFu, fs[c].m - 31)), 155);
        const u128 y = spu_fi(d, spu_frest(d));
        rcp[c] = d._u32[0] ? fma_(fnms_(F(d), F(y), one), F(y), F(y)) : zero;
    }
    for (uint32_t g = 0; g < iters; g++) {
        u128 cb[3], ci[3], cd[3];
        __m128 cv[3];
        uint32_t oaddr[4];
        for (uint32_t k = 0; k < 4; k++) {
            const uint32_t B = bit0 + (4 * g + k) * T;
            const unsigned __int128 win = ea_window(ls, B);
            const uint32_t w1 = (uint32_t)(win >> 96), w2 = (uint32_t)(shlq_(win, T1) >> 96),
                           w3 = (uint32_t)(shlq_(shlq_(win, T1), T2) >> 96);
            ea_comp(w1, f1, &cb[0]._u32[k], &ci[0]._u32[k], &cd[0]._u32[k]);
            ea_comp(w2, f2, &cb[1]._u32[k], &ci[1]._u32[k], &cd[1]._u32[k]);
            ea_comp(w3, f3, &cb[2]._u32[k], &ci[2]._u32[k], &cd[2]._u32[k]);
            oaddr[k] = r5 + 48 * ea_idx(ls, r6, g, k);
        }
        for (int c = 0; c < 3; c++)
            cv[c] = fs[c].e ? F(cb[c]) : fm_(csflt0_(ci[c]), rcp[c]);
        __m128 a = fnms_(cv[0], cv[0], one);
        a = fnms_(cv[1], cv[1], a);
        a = fnms_(cv[2], cv[2], a);
        a = _mm_blendv_ps(a, zero, fcgt_(zero, a));
        const __m128 y0 = F(spu_fi(U(a), spu_frsqest(U(a))));
        const __m128 h = fm_(y0, half), p = fm_(a, y0);
        const __m128 y1 = fma_(h, fnms_(p, y0, one), y0);
        const u128 w = U(fm_(y1, a));
        const u128 v0 = U(cv[0]), v1 = U(cv[1]), v2 = U(cv[2]);
        static const uint8_t order[4] = { 0, 1, 3, 2 };
        for (int t = 0; t < 4; t++) {
            const int l = order[t];
            const uint32_t c1 = v0._u32[l], c2 = v1._u32[l], c3 = v2._u32[l], wl = w._u32[l];
            u128 q;
            /* lane masks of the SPU code: 0xE0000000 << kk, << kk + 1, << kk + 2, bit 31 each */
            const int m1 = kk <= 2, m2 = kk <= 1, m3 = kk == 0;
            const uint32_t t236 = m1 ? c1 : wl, t237 = m1 ? wl : c1;
            const uint32_t t240 = m2 ? c2 : t237, t242 = m2 ? t237 : c2;
            const uint32_t t245 = m3 ? c3 : t242, t246 = m3 ? t242 : c3;
            q._u32[0] = t236;
            q._u32[1] = t240;
            q._u32[2] = t245;
            q._u32[3] = t246;
            stq(ls, oaddr[l], q);
        }
    }
    return 1;
}

/* ---- 0x12438: the keyframe sampler ----
 *
 * Arguments: r17 = element count n (preferred word; 0 returns at once), r3 = four bit addresses (one per lane: the
 * key-presence bitmask of elements 4g + k, each advanced by 4 r20 bits a group), r4-r7 and r8-r11 = per-lane 128-bit
 * frame masks (r4-r7: the frames of the clip, r8-r11: the frames before the current one), r12 = per-lane frame index,
 * r13 = per-lane float (the fraction), r14 / r15 / r16 = byte addresses of three packed streams (the keys, the first
 * values, the last values), r18 = output base, r19 = index table (8 bytes a group, quadword-wrapped, as 0x13100's),
 * r21 = the spec stream (one 32-bit packing spec per element, 16 bytes a group at any byte), r20 preferred word = the
 * bitmask stride in bits, r22 bit 0 = rotations (slerp; else lerp with w = 1). Per group of four elements, in SPU
 * order (this function reads the local store at the same moments as the SPU code, so outputs that land on later
 * inputs read the same values):
 *   spec_k = the 4 words at r21 (unaligned); fields (s, e, m) x 3 and p as 0x13100; T_k = its bit width; E_k = the
 *     exclusive prefix sum of T over the group's lanes, S = the sum. Streams: first_k = 8 r15 + E_k, last_k = 8 r16 + E_k
 *     (both advance by S a group), keys at 8 r14 + (advances by sum_k cA_k T_k a group).
 *   M_k = the 128 bits at bit r3_k; cA = popcount(M & r4-7), cB = popcount(M & r8-11); d_next = tz128(M & r8-11)
 *     - (128 - r12) (tz128: word 3 least significant, 128 when 0), d_prev = lz128((M & r4-7 & ~r8-11) | ~r4-7) + 1
 *     - r12 (word 0 most significant).
 *   key position base_k = keys + cB_k T_k + sum_{j<k} cA_j T_j (16-bit signed products, mpy / mpya);
 *     key A at base - T if cB > 0 else first_k; key B at last_k if cB == cA else base.
 *   Both keys decoded as 0x13100's components (per-lane fields), w = sqrt(1 - |v|^2) inserted at lane p (clamped at 0
 *     for A only, as the SPU code); t = (r13 + d_next) / (d_next + d_prev) (cuflt, frest + fi + one Newton step).
 *   Rotations: d = A.B (x, y, z, w order, fma chain), B and d negated when d < 0; theta = acos(d) as
 *     sqrt(1 - d) (frsqest + fi, times 1 - d) times pi/2 - 0.2146 d + 0.0890 d^2 - 0.0502 d^3 (the image's constants
 *     at LS 0x15E20-0x15E50); sin by the odd series to x^9 (0x15DE0-0x15E10); weights sin((1 - t) theta) / sin theta
 *     and sin(t theta) / sin theta (frest + fi, no Newton step), or 1 - t and t when d > 0.999 (0x3F7FBE77).
 *     Else lerp: (B t + A) - A t per component, w = 1.
 *   Quadword k (x, y, z, w) stored at r18 + 48 idx_k, k = 0..3 in order.
 * Saves r80-r127 and r79 below r1 first and reloads them at the end (as the SPU code: the reload reads what is
 * there then). The constant table (LS 0x15CF0-0x15E6F: shuffle patterns and the series) is checked once: a
 * changed table declines, and so does an output quadword landing on it or on the index table (the only data that
 * steer where later stores go) and more than 64 groups. Bit operations follow the SPU rules as in 0x13100; float
 * work is the runtime's helpers or native_simd.h's exact fast paths, four elements in the four lanes. */
static const uint8_t ea_k12438[0x180] = {   /* LS 0x15CF0-0x15E6F of the job image */
    0x00,0x01,0x02,0x03,0x00,0x01,0x02,0x03,0x00,0x01,0x02,0x03,0x00,0x01,0x02,0x03,
    0x0c,0x0d,0x0e,0x0f,0x0c,0x0d,0x0e,0x0f,0x0c,0x0d,0x0e,0x0f,0x0c,0x0d,0x0e,0x0f,
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
    0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
    0x00,0x01,0x02,0x03,0x10,0x11,0x12,0x13,0x04,0x05,0x06,0x07,0x14,0x15,0x16,0x17,
    0x08,0x09,0x0a,0x0b,0x18,0x19,0x1a,0x1b,0x0c,0x0d,0x0e,0x0f,0x1c,0x1d,0x1e,0x1f,
    0x00,0x01,0x02,0x03,0x10,0x11,0x12,0x13,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
    0x80,0x80,0x00,0x01,0x80,0x80,0x10,0x11,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
    0x02,0x03,0x04,0x05,0x12,0x13,0x14,0x15,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
    0x03,0x07,0x0b,0x0f,0x03,0x07,0x0b,0x0f,0x03,0x07,0x0b,0x0f,0x03,0x07,0x0b,0x0f,
    0x13,0x80,0x80,0x80,0x03,0x17,0x80,0x80,0x03,0x07,0x1b,0x80,0x03,0x07,0x0b,0x1f,
    0xe0,0x00,0x00,0x00,0xe0,0x00,0x00,0x00,0xe0,0x00,0x00,0x00,0xe0,0x00,0x00,0x00,
    0x7f,0xff,0xff,0xff,0x7f,0xff,0xff,0xff,0x7f,0xff,0xff,0xff,0x7f,0xff,0xff,0xff,
    0xbe,0x2a,0xaa,0xab,0xbe,0x2a,0xaa,0xab,0xbe,0x2a,0xaa,0xab,0xbe,0x2a,0xaa,0xab,
    0x3c,0x08,0x88,0x87,0x3c,0x08,0x88,0x87,0x3c,0x08,0x88,0x87,0x3c,0x08,0x88,0x87,
    0xb9,0x50,0x0c,0x03,0xb9,0x50,0x0c,0x03,0xb9,0x50,0x0c,0x03,0xb9,0x50,0x0c,0x03,
    0x36,0x38,0xb9,0x4f,0x36,0x38,0xb9,0x4f,0x36,0x38,0xb9,0x4f,0x36,0x38,0xb9,0x4f,
    0x3f,0xc9,0x0f,0xda,0x3f,0xc9,0x0f,0xda,0x3f,0xc9,0x0f,0xda,0x3f,0xc9,0x0f,0xda,
    0x3e,0x5b,0xbf,0xca,0x3e,0x5b,0xbf,0xca,0x3e,0x5b,0xbf,0xca,0x3e,0x5b,0xbf,0xca,
    0x3d,0xb6,0x3a,0x9e,0x3d,0xb6,0x3a,0x9e,0x3d,0xb6,0x3a,0x9e,0x3d,0xb6,0x3a,0x9e,
    0x3d,0x4d,0x83,0x92,0x3d,0x4d,0x83,0x92,0x3d,0x4d,0x83,0x92,0x3d,0x4d,0x83,0x92,
    0x80,0x80,0x00,0x01,0x80,0x80,0x02,0x03,0x80,0x80,0x04,0x05,0x80,0x80,0x06,0x07,
};
#define EA_K_LO 0x15CF0u
#define EA_K_HI 0x15E70u

static inline uint32_t ea_clz(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32u; }
static inline int32_t ea_mpy(uint32_t a, uint32_t b) { return (int32_t)(int16_t)a * (int32_t)(int16_t)b; }

/* a fixed-point component's 1 / (2^m - 1) depends on m only (the divisor rotm(0x7FFFFFFF, m - 31), cuflt, frest,
 * fi, one Newton step; 0 when the divisor is 0): computed once per m with the same helpers, so the same bits */
static uint32_t ea_rcp_m[32];
static volatile int ea_rcp_ready;
static void ea_rcp_init(void)
{
    const __m128 one = fbits(0x3F800000u);
    for (uint32_t m = 0; m < 32; m++) {
        const u128 d = spu_cuflt(sp(rotm_(0x7FFFFFFFu, m - 31)), 155);
        const u128 y = spu_fi(d, spu_frest(d));
        const u128 r = U(fma_(fnms_(F(d), F(y), one), F(y), F(y)));
        ea_rcp_m[m] = d._u32[0] ? r._u32[0] : 0;
    }
    __atomic_store_n(&ea_rcp_ready, 1, __ATOMIC_RELEASE);
}

/* the three components of the word lanes c1..c3 (one element each) decoded with that lane's fields */
static inline void ea_dec3(const uint32_t w[3][4], const ea_field f[4][3], __m128 out[3])
{
    for (int c = 0; c < 3; c++) {
        u128 b, iv, dv, rc;
        for (int k = 0; k < 4; k++) {
            ea_comp(w[c][k], f[k][c], &b._u32[k], &iv._u32[k], &dv._u32[k]);
            rc._u32[k] = ea_rcp_m[f[k][c].m];
        }
        const __m128 fx = fm_(csflt0_(iv), F(rc));
        __m128i ez;
        { uint32_t e0[4]; for (int k = 0; k < 4; k++) e0[k] = f[k][c].e ? 0u : 0xFFFFFFFFu; memcpy(&ez, e0, 16); }
        out[c] = _mm_blendv_ps(F(b), fx, _mm_castsi128_ps(ez));
    }
}

/* w = sqrt(s) as the SPU code: y0 = fi(frsqest), a y0, y0 / 2, 1 - a y0 y0, y1 = y0 / 2 (..) + y0, y1 a */
static inline __m128 ea_sqrtw(__m128 a)
{
    const __m128 one = fbits(0x3F800000u), half = fbits(0x3F000000u);
    const __m128 y0 = F(spu_fi(U(a), spu_frsqest(U(a))));
    const __m128 p = fm_(a, y0), h = fm_(y0, half);
    return fm_(fma_(h, fnms_(p, y0, one), y0), a);
}

/* the odd series of the image: x (((c9 x^2 + c7) x^2 + c5) x^2 + c3) x^2 + 1) */
static inline __m128 ea_sin(__m128 x, const __m128 c[4])
{
    const __m128 one = fbits(0x3F800000u), x2 = fm_(x, x);
    __m128 r = fma_(c[0], x2, c[1]);
    r = fma_(r, x2, c[2]);
    r = fma_(r, x2, c[3]);
    r = fma_(r, x2, one);
    return fm_(r, x);
}

static int ea_12438(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t n = ctx->gpr[17]._u32[0];
    if (n == 0) return 1;   /* brz $r17: straight to the return, nothing written */
    const uint32_t iters = (n + 3u) >> 2;
    if (iters == 0 || iters > 64) return 0;
    if (memcmp(ls + EA_K_LO, ea_k12438, sizeof ea_k12438)) return 0;
    if (!__atomic_load_n(&ea_rcp_ready, __ATOMIC_ACQUIRE)) ea_rcp_init();
    const uint32_t r1 = ctx->gpr[1]._u32[0], r18 = ctx->gpr[18]._u32[0], r19 = ctx->gpr[19]._u32[0];
    /* where the stores go, from the index table as it is now: none may land on the table itself or the constants
     * (then the table is never rewritten and these are the SPU's addresses) */
    for (uint32_t g = 0; g < iters; g++)
        for (uint32_t k = 0; k < 4; k++) {
            const int64_t o = (int64_t)LSQ(r18 + 48u * ea_idx(ls, r19, g, k));
            if (ovl(o, 16, LSQ(r19), 8 * (int64_t)iters + 16) || ovl(o, 16, EA_K_LO, EA_K_HI - EA_K_LO)) return 0;
        }
    for (int k = 80; k <= 127; k++) stq(ls, r1 - 16 * (uint32_t)(k - 79), ctx->gpr[k]);
    stq(ls, r1 - 0x310u, ctx->gpr[79]);

    const __m128 one = fbits(0x3F800000u), mone = fbits(0xBF800000u), zero = _mm_setzero_ps();
    const __m128 thr = fbits(0x3F7FBE77u);
    const __m128 cs[4] = { fbits(0x3638B94Fu), fbits(0xB9500C03u), fbits(0x3C088887u), fbits(0xBE2AAAABu) };
    const __m128 ca[4] = { fbits(0x3D4D8392u), fbits(0x3DB63A9Eu), fbits(0x3E5BBFCAu), fbits(0x3FC90FDAu) };
    const int rot = ctx->gpr[22]._u32[0] & 1;
    uint32_t r3[4], m4[4][4], m8[4][4], r12[4];
    for (int k = 0; k < 4; k++) {
        r3[k] = ctx->gpr[3]._u32[k];
        r12[k] = ctx->gpr[12]._u32[k];
        for (int j = 0; j < 4; j++) { m4[j][k] = ctx->gpr[4 + j]._u32[k]; m8[j][k] = ctx->gpr[8 + j]._u32[k]; }
    }
    const __m128 r13 = F(ctx->gpr[13]);
    uint32_t keys = ctx->gpr[14]._u32[0] << 3, first = ctx->gpr[15]._u32[0] << 3, last = ctx->gpr[16]._u32[0] << 3;
    const uint32_t step3 = ctx->gpr[20]._u32[0] << 2;
    uint32_t r21 = ctx->gpr[21]._u32[0], r19g = r19;
    for (uint32_t g = 0; g < iters; g++) {
        /* the group's four specs: 16 bytes at r21 (two lqd, shlqby / rotqmby) */
        const uint32_t sh = r21 & 15;
        const uint8_t* q0 = ls + LSQ(r21);
        const uint8_t* q1 = ls + LSQ(r21 + 16);
        uint8_t sb[16];
        for (uint32_t i = 0; i < 16; i++) sb[i] = i + sh < 16 ? q0[i + sh] : q1[i + sh - 16];
        r21 += 16;
        ea_field f[4][3];
        uint32_t spec[4], T[4], T1[4], T2[4], E[4], S = 0;
        for (int k = 0; k < 4; k++) {
            const uint32_t s = be32(sb + 4 * k);
            spec[k] = s;
            f[k][0] = (ea_field){ (s >> 31) & 1, (s >> 27) & 15, (s >> 22) & 31 };
            f[k][1] = (ea_field){ (s >> 21) & 1, (s >> 17) & 15, (s >> 12) & 31 };
            f[k][2] = (ea_field){ (s >> 11) & 1, (s >> 7) & 15, (s >> 2) & 31 };
            T1[k] = f[k][0].s + f[k][0].e + f[k][0].m;
            T2[k] = f[k][1].s + f[k][1].e + f[k][1].m;
            T[k] = T1[k] + T2[k] + f[k][2].s + f[k][2].e + f[k][2].m;
            E[k] = S;
            S += T[k];
        }
        /* bitmasks, key counts and the two distances */
        uint32_t cA[4], cB[4], dn[4], dp[4];
        for (int k = 0; k < 4; k++) {
            const unsigned __int128 W = ea_window(ls, r3[k]);
            uint32_t M[4], ta = 0, tb = 0, v[4], z[4], l[4], y[4];
            for (int j = 0; j < 4; j++) {
                M[j] = (uint32_t)(W >> (96 - 32 * j));
                const uint32_t a = M[j] & m4[j][k], b = M[j] & m8[j][k];
                ta += (uint32_t)__builtin_popcount(a);
                tb += (uint32_t)__builtin_popcount(b);
                const uint32_t c = ea_clz(b & (0u - b));
                z[j] = c == 32;
                v[j] = z[j] ? 32u : 31u - c;
                l[j] = ea_clz((a & ~m8[j][k]) | ~m4[j][k]);
                y[j] = l[j] == 32;
            }
            cA[k] = ta;
            cB[k] = tb;
            const uint32_t tz = v[3] + (z[3] ? v[2] : 0) + (z[3] && z[2] ? v[1] : 0) + (z[3] && z[2] && z[1] ? v[0] : 0);
            const uint32_t lz = l[0] + (y[0] ? l[1] : 0) + (y[0] && y[1] ? l[2] : 0) + (y[0] && y[1] && y[2] ? l[3] : 0);
            dn[k] = tz - (128u - r12[k]);
            dp[k] = lz + 1u - r12[k];
            r3[k] += step3;
        }
        /* the keys' bit positions */
        uint32_t pa[4], pb[4], pre = 0;
        for (int k = 0; k < 4; k++) {
            const uint32_t base = keys + (uint32_t)ea_mpy(cB[k], T[k]) + pre;
            pre += (uint32_t)ea_mpy(cA[k], T[k]);
            pa[k] = cB[k] > 0 ? base - T[k] : first + E[k];
            pb[k] = cB[k] == cA[k] ? last + E[k] : base;
        }
        keys += pre;
        first += S;
        last += S;
        /* t = (r13 + d_next) / (d_next + d_prev) */
        __m128 t;
        {
            u128 un, up;
            for (int k = 0; k < 4; k++) { un._u32[k] = dn[k]; up._u32[k] = dp[k]; }
            const __m128 fn = cuflt0_(un), fp = cuflt0_(up);
            const __m128 num = fa_(r13, fn), den = fa_(fn, fp);
            const __m128 y = F(spu_fi(U(den), spu_frest(U(den))));
            t = fm_(num, fma_(fnms_(den, y, one), y, y));
        }
        /* the two keys */
        __m128 a[3], b[3];
        {
            uint32_t wa[3][4], wb[3][4];
            for (int k = 0; k < 4; k++) {
                const unsigned __int128 A = ea_window(ls, pa[k]), B = ea_window(ls, pb[k]);
                const unsigned __int128 A1 = shlq_(A, T1[k]), B1 = shlq_(B, T1[k]);
                wa[0][k] = (uint32_t)(A >> 96); wa[1][k] = (uint32_t)(A1 >> 96); wa[2][k] = (uint32_t)(shlq_(A1, T2[k]) >> 96);
                wb[0][k] = (uint32_t)(B >> 96); wb[1][k] = (uint32_t)(B1 >> 96); wb[2][k] = (uint32_t)(shlq_(B1, T2[k]) >> 96);
            }
            ea_dec3(wa, f, a);
            ea_dec3(wb, f, b);
        }
        __m128 sA = fnms_(a[0], a[0], one);
        sA = fnms_(a[1], a[1], sA);
        sA = fnms_(a[2], a[2], sA);
        sA = _mm_blendv_ps(sA, zero, fcgt_(zero, sA));
        const __m128 wA = ea_sqrtw(sA);
        __m128 sB = fnms_(b[0], b[0], one);
        sB = fnms_(b[1], b[1], sB);
        sB = fnms_(b[2], b[2], sB);
        const __m128 wB = ea_sqrtw(sB);
        /* w into lane p = spec & 3: masks bit 31 of 0xE0000000 << (~spec & 3), << 1, << 2 */
        __m128 m1, m2, m3;
        {
            uint32_t x1[4], x2[4], x3[4];
            for (int k = 0; k < 4; k++) {
                const uint32_t m = 0xE0000000u << (~spec[k] & 3);
                x1[k] = (m >> 31) ? ~0u : 0; x2[k] = (m >> 30 & 1) ? ~0u : 0; x3[k] = (m >> 29 & 1) ? ~0u : 0;
            }
            memcpy(&m1, x1, 16); memcpy(&m2, x2, 16); memcpy(&m3, x3, 16);
        }
        __m128 qa[4], qb[4];
        {
            const __m128 r16 = _mm_blendv_ps(a[0], wA, m1), r15 = _mm_blendv_ps(a[1], r16, m2);
            qa[0] = _mm_blendv_ps(wA, a[0], m1);
            qa[1] = _mm_blendv_ps(r16, a[1], m2);
            qa[2] = _mm_blendv_ps(r15, a[2], m3);
            qa[3] = _mm_blendv_ps(a[2], r15, m3);
            const __m128 r2 = _mm_blendv_ps(b[0], wB, m1), r14 = _mm_blendv_ps(b[1], r2, m2);
            qb[0] = _mm_blendv_ps(wB, b[0], m1);
            qb[1] = _mm_blendv_ps(r2, b[1], m2);
            qb[2] = _mm_blendv_ps(r14, b[2], m3);
            qb[3] = _mm_blendv_ps(b[2], r14, m3);
        }
        __m128 o[4];
        if (rot) {
            const __m128 d = fma_(qa[3], qb[3], fma_(qa[2], qb[2], fma_(qa[1], qb[1], fm_(qa[0], qb[0]))));
            const __m128 sg = _mm_blendv_ps(one, mone, fcgt_(zero, d));
            __m128 bs[4];
            for (int c = 0; c < 4; c++) bs[c] = fm_(qb[c], sg);
            const __m128 ad = fm_(d, sg);
            const __m128 om = fs_(one, ad);
            const __m128 ry = F(spu_fi(U(om), spu_frsqest(U(om))));
            const __m128 sq = fnms_(ry, ad, ry);
            __m128 p = fm_(ad, ca[0]);
            p = fnms_(ad, p, ca[1]);
            p = fnms_(ad, p, ca[2]);
            p = fnms_(ad, p, ca[3]);
            const __m128 th = fm_(p, sq);
            const __m128 sth = ea_sin(th, cs);
            const __m128 stt = ea_sin(fm_(t, th), cs);
            const __m128 omt = fs_(one, t);
            const __m128 sot = ea_sin(fm_(omt, th), cs);
            const __m128 ri = F(spu_fi(U(sth), spu_frest(U(sth))));
            const __m128 lm = fcgt_(ad, thr);
            const __m128 wa = _mm_blendv_ps(fm_(sot, ri), omt, lm), wb = _mm_blendv_ps(fm_(stt, ri), t, lm);
            for (int c = 0; c < 4; c++) o[c] = fma_(bs[c], wb, fm_(qa[c], wa));
        } else {
            for (int c = 0; c < 3; c++) o[c] = fnms_(qa[c], t, fma_(qb[c], t, qa[c]));
            o[3] = one;
        }
        /* element k's (x, y, z, w) at r18 + 48 idx_k, k = 0..3 in order */
        const u128 ox = U(o[0]), oy = U(o[1]), oz = U(o[2]), ow = U(o[3]);
        for (uint32_t k = 0; k < 4; k++) {
            u128 q;
            q._u32[0] = ox._u32[k]; q._u32[1] = oy._u32[k]; q._u32[2] = oz._u32[k]; q._u32[3] = ow._u32[k];
            stq(ls, r18 + 48u * ea_idx(ls, r19g, 0, k), q);
        }
        r19g += 8;
    }
    for (int k = 80; k <= 127; k++) ctx->gpr[k] = ldq(ls, r1 - 16 * (uint32_t)(k - 79));
    ctx->gpr[79] = ldq(ls, r1 - 0x310u);
    return 1;
}

const kz2_native_entry kz2_native_edgeanim[] = {
    { 75, "KZ2_NATIVE_JOB_EDGEANIM", "Anim.EdgeAnim", "job_00d95d80", 0x13100u, ea_13100, "ea_13100", 0, 0, 0, 0 },
    { 76, "KZ2_NATIVE_JOB_EDGEANIM_KEYS|KZ2_NATIVE_JOB_EDGEANIM", "Anim.EdgeAnim", "job_00d95d80", 0x12438u, ea_12438, "ea_12438", 0, 0, 0, 0 },
};
const unsigned kz2_native_edgeanim_n = sizeof kz2_native_edgeanim / sizeof kz2_native_edgeanim[0];
