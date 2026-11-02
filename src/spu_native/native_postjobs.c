/* From Project Pyrrhus by undecuple (https://github.com/undecuple/project-pyrrhus, port/kz2/spu/native_postjobs.c at 218f660),
 * MIT License, Copyright (c) 2026 undecuple -- see THIRD_PARTY_NOTICES.md. Tokisaki changes are marked "tokisaki:". */
/* native_postjobs.c -- the small lifted Gfx.Post jobs as native host code:
 * Gfx.Post.Modulate, BloomIntegrate, Vignette and DepthToFuzzy (each job's whole pass function @0x4A20),
 * Gfx.Post.Downsample (its two row kernels @0x5620, @0x56D0), Gfx.Post.Zero (its memset @0x5698) and Gfx.Post.ILR
 * (its pass's row loop @0x4E20 to the return). Default off: KZ2_NATIVE_JOB_<NAME>=1 per job (MODULATE, BLOOMINT,
 * VIGNETTE, DEPTHTOFUZZY, DOWNSAMPLE, ZERO, ILR), or KZ2_NATIVE_JOB_POST=1 (every Gfx.Post gate).
 *
 * Written from the disassembly and tools/spu/spu_symtrace.py's per-store dataflow of each pass (the RE notes
 * "An earlier change"). Every float operation is the runtime helper's own fast path inline, or the helper itself for any
 * vector its fast path would not take (native_simd.h), applied in the SPU code's order: the same bits as the lifted
 * code by construction. Integer work is exact. Where the SPU code runs one pixel per vector (a pixel's four channels
 * in the four lanes) the native code may run four pixels per vector (one channel of four pixels): the helpers are
 * lane-wise.
 *
 * Common layout of the passes (the job body @0x4330 calls them once, with the parameter block at r3): the quadword
 * at r3 + 0x10 holds the width (byte 4) and the rows (byte 5); a pass covers rows x floor(width / 20) x 20 pixels
 * back to back (the SPU computes width / 20 with a 0xCCCCCCCD multiply and masks it with 255: the same value for a
 * byte). The passes are leaves without channel access and without stack stores; post_kernels.py --live finds no
 * scratch register read after the call @0x4330 in any of the four images, nor after Downsample's kernel calls
 * @0x4AD4 and @0x4B4C. The native code leaves r2-r79 as they were (r0, r1, r80-r127 are not touched by the SPU code
 * either). Each declines (the lifted body runs) on arguments outside what it was written for: misaligned buffers,
 * ranges that wrap the local store, outputs that overlap inputs other than exactly in place where that is safe.
 */
#include "native_simd.h"

/* rows x floor(width / 20) x 20 pixels from the parameter block at r3 */
static uint32_t pass_pixels(const uint8_t* ls, uint32_t prm, uint32_t* width, uint32_t* rows)
{
    const uint8_t* q = ls + LSQ(prm + 0x10);
    *width = q[4];
    *rows = q[5];
    return *rows * (*width / 20) * 20;
}

/* An output range against an input range: disjoint, or (same_ok) exactly the same bytes. */
static int io_ok(int64_t out, int64_t on, int64_t in, int64_t inn, int same_ok)
{
    if (out + on > SPU_LS_SIZE || in + inn > SPU_LS_SIZE) return 0;
    if (!ovl(out, on, in, inn)) return 1;
    return same_ok && out == in && on == inn;
}

/* ---- Gfx.Post.Modulate @0x4A20 (job_00d56000) --------------------------------------------------------------------
 * r4 = the factor quadword (its four float lanes, one per channel), r5 = source pixels, r9 = output. The factors are
 * m_c = cfltu(f_c, 165) (8.8 fixed point; spu_cfltu itself, once per call). Format: the halfword at bytes 6-7 of
 * r3 + 0x10 (4 = RGBA8, else 8-byte pixels of four big-endian 16-bit channels).
 *   RGBA8 (loop 0x4AC8):  out_c = low byte of ((x_c * s16(m_c)) >> 8)   (mpy: signed 16 x 16; rotmi: logical)
 *   16-bit (loop 0x4D00): out_c = min((s16(x_c) * s16(m_c)) >> 8, 0xFFFF)  (32-bit product, logical shift, clgt)
 * Each output quadword depends only on the input quadword at the same offset, loaded before it is stored: in place
 * (output = source) is the same as apart. */
#define MOD_POOL_LO 0x5BF0u
#define MOD_POOL_HI 0x5C90u

static int modulate_4A20(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t prm = ctx->gpr[3]._u32[0], src = ctx->gpr[5]._u32[0], dst = ctx->gpr[9]._u32[0];
    uint32_t width, rows;
    const uint32_t n = pass_pixels(ls, prm, &width, &rows);
    const uint8_t* q = ls + LSQ(prm + 0x10);
    const uint32_t P = ((uint32_t)q[6] << 8 | q[7]) == 4 ? 4 : 8;
    if ((src | dst) & 15) return 0;
    if (!io_ok(dst, (int64_t)P * n, src, (int64_t)P * n, 1)) return 0;
    if (!n) return 1;
    const u128 m = spu_cfltu(ldq(ls, ctx->gpr[4]._u32[0]), 165);
    if (P == 4) {
        /* 16-bit lanes: the low 16 bits of x * m are the low 16 bits of the 32-bit product; bits 8-15 are the byte */
        const __m128i m16 = _mm_set_epi16((short)m._u32[3], (short)m._u32[2], (short)m._u32[1], (short)m._u32[0],
                                          (short)m._u32[3], (short)m._u32[2], (short)m._u32[1], (short)m._u32[0]);
        const __m128i z = _mm_setzero_si128();
        for (uint32_t g = 0; g < n / 4; g++) {
            const __m128i px = _mm_loadu_si128((const __m128i*)(ls + src + 16 * g));
            const __m128i lo = _mm_srli_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(px, z), m16), 8);
            const __m128i hi = _mm_srli_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(px, z), m16), 8);
            _mm_storeu_si128((__m128i*)(ls + dst + 16 * g), _mm_packus_epi16(lo, hi));
        }
    } else {
        const __m128i ms = _mm_set_epi32((int16_t)m._u32[3], (int16_t)m._u32[2], (int16_t)m._u32[1], (int16_t)m._u32[0]);
        /* one pixel = four big-endian halfwords -> four sign-extended 32-bit lanes */
        const __m128i ex = _mm_set_epi8(-1, -1, 6, 7, -1, -1, 4, 5, -1, -1, 2, 3, -1, -1, 0, 1);
        const __m128i pk = _mm_set_epi8(-1, -1, -1, -1, -1, -1, -1, -1, 12, 13, 8, 9, 4, 5, 0, 1);
        const __m128i cap = _mm_set1_epi32(0xFFFF);
        for (uint32_t x = 0; x < n; x++) {
            const __m128i h = _mm_shuffle_epi8(_mm_loadl_epi64((const __m128i*)(ls + src + 8 * x)), ex);
            const __m128i hs = _mm_srai_epi32(_mm_slli_epi32(h, 16), 16);
            const __m128i v = _mm_min_epu32(_mm_srli_epi32(_mm_mullo_epi32(hs, ms), 8), cap);
            _mm_storel_epi64((__m128i*)(ls + dst + 8 * x), _mm_shuffle_epi8(v, pk));
        }
    }
    return 1;
}

/* ---- Gfx.Post.BloomIntegrate @0x4A20 (job_00d62e80) --------------------------------------------------------------
 * r4 = address of the scale word S (read as lqd + rotqby by r4), r5 = source, r9 = RGBA8 output. Source format: byte
 * 7 of the quadword at r3 (4 = RGBA8, else 8-byte pixels of 16-bit channels, each taken >> 4).
 *   K = fma(fnms(q, 255, S), e, q), q = fm(S, e), e = fi(255, frest(255))           (S / 255, one Newton step)
 *   per pixel: v_c = clamp(fm(cuflt(u_c), K), 0, 1)   (u_c = byte, or halfword >> 4: below 2^24, cuflt exact)
 *              lum = fa(fa(fa(fm(v_0, 0), fm(v_1, 0.3)), fm(v_2, 0.59)), fm(v_3, 0.11))   (the pool's weights)
 *              out_0 = low byte of cfltu(fm(lum, 255), 173), out_c = low byte of cfltu(fm(v_c, 255), 173), c >= 1
 * The SPU code runs the lum sum across the lanes of a pixel's vector (rotations, lane 0 kept); here lane = pixel. */
#define BINT_POOL_LO 0x65C0u
#define BINT_POOL_HI 0x6680u

static int bloomint_4A20(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t prm = ctx->gpr[3]._u32[0], src = ctx->gpr[5]._u32[0], dst = ctx->gpr[9]._u32[0];
    uint32_t width, rows;
    const uint32_t n = pass_pixels(ls, prm, &width, &rows);
    const uint32_t P = ls[LSQ(prm) + 7] == 4 ? 4 : 8;
    if ((src | dst) & 15) return 0;
    if (!io_ok(dst, 4 * (int64_t)n, src, (int64_t)P * n, P == 4)) return 0;
    if (!n) return 1;
    const u128 k255 = sp(0x437F0000u), e = spu_fi(k255, spu_frest(k255));
    const u128 S = sp(rdw(ls, ctx->gpr[4]._u32[0]));
    const u128 qq = spu_fm(S, e);
    const __m128 K = F(spu_fma(spu_fnms(qq, k255, S), e, qq));
    const __m128 one = _mm_set1_ps(1.0f), f255 = _mm_set1_ps(255.0f), zero = _mm_setzero_ps();
    const __m128 w1 = fbits(0x3E999999u), w2 = fbits(0x3F170A3Du), w3 = fbits(0x3DE147AEu);
    /* channel c of four pixels as four 32-bit lanes (lane = pixel) */
    const __m128i ch4[4] = { _mm_set_epi8(-1, -1, -1, 12, -1, -1, -1, 8, -1, -1, -1, 4, -1, -1, -1, 0),
                             _mm_set_epi8(-1, -1, -1, 13, -1, -1, -1, 9, -1, -1, -1, 5, -1, -1, -1, 1),
                             _mm_set_epi8(-1, -1, -1, 14, -1, -1, -1, 10, -1, -1, -1, 6, -1, -1, -1, 2),
                             _mm_set_epi8(-1, -1, -1, 15, -1, -1, -1, 11, -1, -1, -1, 7, -1, -1, -1, 3) };
    /* 8-byte pixels: channel c (big-endian halfword at 2c) of the two pixels of a quadword, in lanes 0-1 */
    const __m128i ch8[4] = { _mm_set_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 8, 9, -1, -1, 0, 1),
                             _mm_set_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 10, 11, -1, -1, 2, 3),
                             _mm_set_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 12, 13, -1, -1, 4, 5),
                             _mm_set_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 14, 15, -1, -1, 6, 7) };
    for (uint32_t g = 0; g < n / 4; g++) {
        __m128i u[4];
        if (P == 4) {
            const __m128i px = _mm_loadu_si128((const __m128i*)(ls + src + 16 * g));
            for (int c = 0; c < 4; c++) u[c] = _mm_shuffle_epi8(px, ch4[c]);
        } else {
            const __m128i a = _mm_loadu_si128((const __m128i*)(ls + src + 32 * g));
            const __m128i b = _mm_loadu_si128((const __m128i*)(ls + src + 32 * g + 16));
            for (int c = 0; c < 4; c++)
                u[c] = _mm_srli_epi32(_mm_unpacklo_epi64(_mm_shuffle_epi8(a, ch8[c]), _mm_shuffle_epi8(b, ch8[c])), 4);
        }
        __m128 v[4];
        for (int c = 0; c < 4; c++) v[c] = clamp_(fm_(_mm_cvtepi32_ps(u[c]), K), one);
        const __m128 lum = fa_(fa_(fa_(fm_(v[0], zero), fm_(v[1], w1)), fm_(v[2], w2)), fm_(v[3], w3));
        __m128i o[4];
        o[0] = cfltu_(fm_(lum, f255), 173);
        for (int c = 1; c < 4; c++) o[c] = cfltu_(fm_(v[c], f255), 173);
        const __m128i m8 = _mm_set1_epi32(0xFF);
        const __m128i r = _mm_or_si128(_mm_or_si128(_mm_and_si128(o[0], m8), _mm_slli_epi32(_mm_and_si128(o[1], m8), 8)),
                                       _mm_or_si128(_mm_slli_epi32(_mm_and_si128(o[2], m8), 16), _mm_slli_epi32(o[3], 24)));
        _mm_storeu_si128((__m128i*)(ls + dst + 16 * g), r);   /* host bytes = the pixels' bytes in order */
    }
    return 1;
}

/* ---- Gfx.Post.Vignette @0x4A20 (job_00d68f00) ---------------------------------------------------------------------
 * r5 = source RGBA8 pixels (read byte by byte), r9 = output words (one word store per pixel: a read-modify-write of
 * its quadword), width x rows pixels (no multiple of 20 here), one pixel per loop step (loop 0x4AE0).
 *   dx = 2 / width, dy = 2 / rows (frest, fi, one Newton step: fma(fnms(t, w, 2), r, t), t = fm(2, r))
 *   p = the pool quadword @0x58B0 with lane 0 = -1 at each row start; per pixel: s = fa(fa(x^2, y^2), z^2) (fm),
 *   f = clamp(fnms(s^2, s^2, 1), 0, 1); o_c = fm(fm(fm(cuflt(b_c), 1/255), f), 255);
 *   word = (cfltu(o_1) << 16) | (cfltu(o_2) << 8) | cfltu(o_3)  (scale 2^0; channel 0 is cleared)
 *   then p = fa(p, (dx, 0, 0, 0)); after the row p = fa(p, (0, dy, 0, 0)).
 * The cwd masks come from r1 (the stack pointer: 16-byte aligned). The native code keeps p's update chain per pixel
 * (one add) and runs the rest four pixels a vector. In place (output = source) is the same as the SPU's order (each
 * pixel's bytes are read before its own word is stored, nothing else of it); any other overlap runs the lifted code. */
#define VIG_POOL_LO 0x5890u
#define VIG_POOL_HI 0x58C0u

static inline u128 newton2(uint32_t v)   /* 2 / cuflt(v), as the pass computes it */
{
    const u128 two = sp(0x40000000u), w = spu_cuflt(sp(v), 155);
    const u128 r = spu_fi(w, spu_frest(w)), t = spu_fm(two, r);
    return spu_fma(spu_fnms(t, w, two), r, t);
}

static int vignette_4A20(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t prm = ctx->gpr[3]._u32[0], src = ctx->gpr[5]._u32[0], dst = ctx->gpr[9]._u32[0];
    const uint8_t* q = ls + LSQ(prm + 0x10);
    const uint32_t width = q[4], rows = q[5], n = width * rows;
    if (ctx->gpr[1]._u32[0] & 15) return 0;
    if (rows == 0) return 1;
    if ((int64_t)src + 4 * (int64_t)n > SPU_LS_SIZE || (int64_t)(dst & ~3u) + 4 * (int64_t)n > SPU_LS_SIZE) return 0;
    if (ovl(dst & ~3u, 4 * (int64_t)n, src, 4 * (int64_t)n) && !(src == dst && !(src & 3))) return 0;
    const u128 dyq = newton2(rows), dxq = newton2(width);
    const __m128 ddx = _mm_castsi128_ps(_mm_set_epi32(0, 0, 0, (int)dxq._u32[0]));
    const __m128 ddy = _mm_castsi128_ps(_mm_set_epi32(0, 0, (int)dyq._u32[0], 0));
    const __m128 kn = fbits(0x3B808080u), one = _mm_set1_ps(1.0f), f255 = _mm_set1_ps(255.0f);
    __m128 p = F(ldq(ls, VIG_POOL_HI - 16));   /* the quadword @0x58B0 */
    float xs[4], ys[4], zs[4];
    for (uint32_t r = 0; r < rows; r++) {
        p = _mm_blend_ps(p, _mm_set1_ps(-1.0f), 1);   /* selb with fsmbi 0xF000: lane 0 = -1 */
        for (uint32_t i = 0; i < width; i += 4) {
            const uint32_t k = width - i < 4 ? width - i : 4;
            for (uint32_t j = 0; j < 4; j++) {   /* pixel i + j's p, then the per-pixel add (lanes past k: unused) */
                float t[4];
                _mm_storeu_ps(t, p);
                xs[j] = t[0];
                ys[j] = t[1];
                zs[j] = t[2];
                if (j + 1 < k) p = fa_(p, ddx);
            }
            p = fa_(p, ddx);   /* after the group's last pixel */
            const __m128 X = _mm_loadu_ps(xs), Y = _mm_loadu_ps(ys), Z = _mm_loadu_ps(zs);
            const __m128 s = fa_(fa_(fm_(X, X), fm_(Y, Y)), fm_(Z, Z));
            const __m128 s2 = fm_(s, s);
            const __m128 f = clamp_(fnms_(s2, s2, one), one);
            const uint32_t base = src + 4 * (r * width + i);
            __m128i b[4];
            for (int c = 1; c < 4; c++) {
                uint32_t bb[4] = { 0, 0, 0, 0 };
                for (uint32_t j = 0; j < k; j++) bb[j] = ls[LSM(base + 4 * j + (uint32_t)c)];
                b[c] = _mm_loadu_si128((const __m128i*)bb);
            }
            __m128i o[4];
            for (int c = 1; c < 4; c++) o[c] = cfltu_(fm_(fm_(fm_(_mm_cvtepi32_ps(b[c]), kn), f), f255), 173);
            uint32_t w[4];
            _mm_storeu_si128((__m128i*)w, _mm_or_si128(_mm_slli_epi32(o[2], 8), _mm_or_si128(_mm_slli_epi32(o[1], 16), o[3])));
            for (uint32_t j = 0; j < k; j++) {
                uint8_t* d = ls + LSM((dst + 4 * (r * width + i + j)) & ~3u);
                d[0] = (uint8_t)(w[j] >> 24); d[1] = (uint8_t)(w[j] >> 16); d[2] = (uint8_t)(w[j] >> 8); d[3] = (uint8_t)w[j];
            }
        }
        p = fa_(p, ddy);
    }
    return 1;
}

/* ---- Gfx.Post.DepthToFuzzy @0x4A20 (job_00d6fe00) -----------------------------------------------------------------
 * r4 = three words a, b, c (each read as lqd/lqx + rotqby: rdw), r5 = floats d, r9 = output words.
 *   per float: word = cfltu(fm(clamp(fma(a, d, b), 0, 1), fm(fm(c, 255), 2^-8)), 165) << 24
 * Each output quadword depends only on the input quadword at the same offset: in place is the same as apart. */
static int depthfuzzy_4A20(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t prm = ctx->gpr[3]._u32[0], tab = ctx->gpr[4]._u32[0], src = ctx->gpr[5]._u32[0];
    const uint32_t dst = ctx->gpr[9]._u32[0];
    uint32_t width, rows;
    const uint32_t n = pass_pixels(ls, prm, &width, &rows);
    if ((src | dst) & 15) return 0;
    if (!io_ok(dst, 4 * (int64_t)n, src, 4 * (int64_t)n, 1)) return 0;
    if (!n) return 1;
    const __m128 a = F(sp(rdw(ls, tab))), b = F(sp(rdw(ls, tab + 4)));
    const __m128 c = F(spu_fm(spu_fm(sp(rdw(ls, tab + 8)), sp(0x437F0000u)), sp(0x3B800000u)));
    const __m128 one = _mm_set1_ps(1.0f);
    for (uint32_t g = 0; g < n / 4; g++) {
        const __m128 d = _mm_castsi128_ps(bswap32x4(_mm_loadu_si128((const __m128i*)(ls + src + 16 * g))));
        const __m128i o = _mm_slli_epi32(cfltu_(fm_(clamp_(fma_(a, d, b), one), c), 165), 24);
        _mm_storeu_si128((__m128i*)(ls + dst + 16 * g), bswap32x4(o));
    }
    return 1;
}

/* ---- Gfx.Post.Downsample @0x5620 and @0x56D0 (job_00d50280) -------------------------------------------------------
 * r3 = output row, r4 = source row 0, r5 = source stride (row 1 = r4 + r5), r6 = output pixels (4 a step @0x5620,
 * 2 a step @0x56D0; 0 or a remainder never ends the SPU loop). Source quadwords Q0, Q1 (row 0) and R0, R1 (row 1) at
 * 32 j and 32 j + 16 for output quadword j:
 *   @0x5620 (RGBA8):   t1 = avgb(Q0, R0), t2 = avgb(Q1, R1); out = avgb(words 0, 2 of t1 and t2, words 1, 3)
 *                      (a 2x2 box with the SPU's rounding average twice)
 *   @0x56D0 (16-bit):  per row, channels as 32-bit lanes: h = (((A0 + B0) >> 1) + ((A1 + B1) >> 1)) >> 1 with A, B the
 *                      row's Q0, Q1 and 0, 1 the quadword's two pixels; out = row 0's h then row 1's h (low halfwords)
 * The loops load ahead of their stores (software pipelined): the output must not overlap the source rows. */
#define DS4_POOL_LO 0x5A60u
#define DS4_POOL_HI 0x5A80u
#define DS8_POOL_LO 0x5A20u
#define DS8_POOL_HI 0x5A60u

static int ds_args(spu_context* ctx, uint32_t step, uint32_t* dst, uint32_t* r0, uint32_t* r1, uint32_t* nq)
{
    *dst = ctx->gpr[3]._u32[0];
    *r0 = ctx->gpr[4]._u32[0];
    *r1 = *r0 + ctx->gpr[5]._u32[0];
    const uint32_t n = ctx->gpr[6]._u32[0];
    if (n == 0 || n % step || n > 4096 || ((*dst | *r0 | *r1) & 15)) return 0;
    *nq = n / step;   /* output quadwords */
    const int64_t in = 32 * (int64_t)*nq;
    if ((int64_t)*dst + 16 * (int64_t)*nq > SPU_LS_SIZE || (int64_t)*r0 + in > SPU_LS_SIZE || (int64_t)*r1 + in > SPU_LS_SIZE)
        return 0;
    return !ovl(*dst, 16 * (int64_t)*nq, *r0, in) && !ovl(*dst, 16 * (int64_t)*nq, *r1, in);
}

static int down4_5620(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    uint32_t dst, r0, r1, nq;
    if (!ds_args(ctx, 4, &dst, &r0, &r1, &nq)) return 0;
    for (uint32_t j = 0; j < nq; j++) {
        const __m128i t1 = _mm_avg_epu8(_mm_loadu_si128((const __m128i*)(ls + r0 + 32 * j)),
                                        _mm_loadu_si128((const __m128i*)(ls + r1 + 32 * j)));
        const __m128i t2 = _mm_avg_epu8(_mm_loadu_si128((const __m128i*)(ls + r0 + 32 * j + 16)),
                                        _mm_loadu_si128((const __m128i*)(ls + r1 + 32 * j + 16)));
        const __m128 a = _mm_castsi128_ps(t1), b = _mm_castsi128_ps(t2);
        const __m128i ev = _mm_castps_si128(_mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0)));
        const __m128i od = _mm_castps_si128(_mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1)));
        _mm_storeu_si128((__m128i*)(ls + dst + 16 * j), _mm_avg_epu8(ev, od));
    }
    return 1;
}

static inline __m128i ds8_row(const uint8_t* q0, const uint8_t* q1)
{
    const __m128i p0 = _mm_set_epi8(-1, -1, 6, 7, -1, -1, 4, 5, -1, -1, 2, 3, -1, -1, 0, 1);
    const __m128i p1 = _mm_set_epi8(-1, -1, 14, 15, -1, -1, 12, 13, -1, -1, 10, 11, -1, -1, 8, 9);
    const __m128i a = _mm_loadu_si128((const __m128i*)q0), b = _mm_loadu_si128((const __m128i*)q1);
    const __m128i h0 = _mm_srli_epi32(_mm_add_epi32(_mm_shuffle_epi8(a, p0), _mm_shuffle_epi8(b, p0)), 1);
    const __m128i h1 = _mm_srli_epi32(_mm_add_epi32(_mm_shuffle_epi8(a, p1), _mm_shuffle_epi8(b, p1)), 1);
    return _mm_srli_epi32(_mm_add_epi32(h0, h1), 1);
}

static int down8_56D0(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    uint32_t dst, r0, r1, nq;
    if (!ds_args(ctx, 2, &dst, &r0, &r1, &nq)) return 0;
    const __m128i pk = _mm_set_epi8(-1, -1, -1, -1, -1, -1, -1, -1, 12, 13, 8, 9, 4, 5, 0, 1);
    for (uint32_t j = 0; j < nq; j++) {
        const __m128i a = _mm_shuffle_epi8(ds8_row(ls + r0 + 32 * j, ls + r0 + 32 * j + 16), pk);
        const __m128i b = _mm_shuffle_epi8(ds8_row(ls + r1 + 32 * j, ls + r1 + 32 * j + 16), pk);
        _mm_storeu_si128((__m128i*)(ls + dst + 16 * j), _mm_unpacklo_epi64(a, b));
    }
    return 1;
}

/* ---- Gfx.Post.Zero: memset @0x5698 (job_00d4c580) ---------------------------------------------------------------
 * The pass @0x4A20 tail-branches here with r3 = r9 (the output), r4 = 0 and r5 = width x rows x the format halfword
 * (mpyu of the low halfwords); r0 still returns to the job body (@0x4330: post_kernels.py --live finds no scratch
 * register read after that call; 0x4A20 is this routine's only caller). r3 = destination, r4 = fill (byte 3 of the
 * preferred word, splatted by shufb 0x0303), r5 = bytes: whole quadwords, then the first r5 & 15 bytes of the next
 * (selb with a rotqmby mask: a read-modify-write). r3 is returned unchanged. A destination that is not 16-byte aligned
 * takes another path in the SPU code (@0x5700): the native code declines it. */
static int zero_memset_5698(spu_context* ctx)
{
    const uint32_t dst = ctx->gpr[3]._u32[0], size = ctx->gpr[5]._u32[0];
    if (!size) return 1;
    if ((dst & 15) || (int64_t)dst + size > SPU_LS_SIZE) return 0;
    memset(ctx->ls + dst, (int)(ctx->gpr[4]._u32[0] & 0xFF), size);
    return 1;
}

/* ---- Gfx.Post.ILR: the pass's row loop @0x4E20 to its return (job_00d65780) -------------------------------------
 * A radial blur toward the image centre: 8 taps per pixel at the pixel's normalised position scaled by 8 constants,
 * integer weights per tap and channel (the RE notes "An earlier change"; tools/spu/spu_symtrace.py --cfloat/--regfile over
 * one iteration of loop 0x4E28, 20 pixels in five quadwords). The gate sits at 0x4E20, the row-loop head the prologue
 * @0x4A20 hops to once its stack frame is built (r1 lowered by 880: r80-r127 saved, locals at r1 + 0x20 .. 0x60) and its
 * constants are in registers: the native code reads them from there, so the prologue (once per pass) stays lifted.
 *   per row:    for the row's G = [r1 + 0x40] iterations of 20 pixels, then X = fa(X, -2), Y = fa(Y, [r1 + 0x20]),
 *               [r1 + 0x30] += 1 (every lane); rows until [r1 + 0x30] equals [r1 + 0x50] (preferred words)
 *   per quadword q of 4 pixels (lane j = pixel j), X the running x vector (r36, then fa(X, r71) per quadword):
 *               tap k = 0..7: dy = clamp(cflts(fm(Y, r75+k)), r60, r61), dx = clamp(cflts(fm(X, r63+k)), r55, r56)
 *               (signed: cgt + selb against the high bound, then the low); a = s16(dy) s16(r58) + s16(dx) s16(r57);
 *               the tap pixel = bytes (a + c) & 15, c = 0..3, of the quadword at (centre + a) & ~15 (lqx + rotqby by
 *               a & 15; centre = the preferred word of [r1 + 0x60])
 *               S_c = sum over k of byte_c x w_kc (mpy/mpya: signed 16-bit products, 32-bit sum); out byte 4 j + c =
 *               bits 8-15 of (S_c > r59 (unsigned) ? r59 : S_c); 16 bytes to r49 + 16 q, r49 advancing by 80 per iteration
 *   weights:    the pool quadwords 0x7420, 0x7410, 0x7460, 0x7450, 0x7440, 0x7400, 0x7430, 0x7400 for taps 0-7 (lqr in the
 *               loop), or (some pixels) register copies the
 *               prologue made of them: the native code checks every copy against the pool and declines if one differs
 *   return:     r1 += 880, r80-r127 reloaded from the frame (the epilogue @0x66A8), then the gate returns to r0
 * Scratch registers are left as they are (post_kernels.py --live: the call @0x4330 reads none after it). The native
 * code declines on a frame not 16-byte aligned, pool masks or the clamp register not what the code was written for,
 * a row or iteration count out of range, or an output overlapping the frame or the taps' reach. */
#define ILR_POOL_LO 0x7400u
#define ILR_POOL_HI 0x7500u
static const uint8_t ilr_wreg[5][4][8] = {
    { { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { { 0, 0, 0, 0, 0, 0, 0, 0 }, { 125, 127, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0, 0 }, { 124, 126, 0, 0, 0, 0, 0, 0 } },
    { { 0, 0, 0, 0, 0, 0, 0, 0 }, { 109, 111, 113, 115, 117, 119, 121, 123 }, { 0, 0, 0, 0, 0, 0, 0, 0 }, { 108, 110, 112, 114, 116, 118, 120, 122 } },
    { { 0, 0, 0, 0, 0, 0, 0, 0 }, { 93, 95, 97, 99, 101, 103, 105, 107 }, { 0, 0, 0, 0, 0, 0, 0, 0 }, { 92, 94, 96, 98, 100, 102, 104, 106 } },
    { { 0, 0, 0, 0, 0, 0, 0, 0 }, { 83, 84, 85, 86, 87, 88, 89, 91 }, { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0, 90 } },
};

static inline __m128i cflts_(__m128 x)   /* spu_cflts(x, 173): truncation, the helper for exponent 255 or |x| >= 2^31 */
{
    const __m128 ax = _mm_andnot_ps(_mm_set1_ps(-0.0f), x);
    if (__builtin_expect(e255(x) | _mm_movemask_ps(_mm_cmpge_ps(ax, _mm_set1_ps(2147483648.0f))), 0)) {
        const u128 r = spu_cflts(U(x), 173);
        __m128i v;
        memcpy(&v, &r, 16);
        return v;
    }
    return _mm_cvttps_epi32(x);
}

static inline __m128i s16x(__m128i v) { return _mm_srai_epi32(_mm_slli_epi32(v, 16), 16); }
static inline __m128i qv(u128 x) { __m128i r; memcpy(&r, &x, 16); return r; }

static int ilr_4E20(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t r1 = ctx->gpr[1]._u32[0];
    if ((r1 & 15) || r1 + 880 > SPU_LS_SIZE) return 0;
    for (int k = 0; k < 4; k++)
        if (ctx->gpr[59]._u32[k] != 0xFFFFu) return 0;
    /* weights: the pool's, and every register copy the loop reads equal to its tap's pool quadword */
    __m128i w[8];
    static const uint16_t wq[8] = { 0x7420, 0x7410, 0x7460, 0x7450, 0x7440, 0x7400, 0x7430, 0x7400 };
    for (int k = 0; k < 8; k++) w[k] = qv(ldq(ls, wq[k]));
    for (int q = 0; q < 5; q++)
        for (int p = 0; p < 4; p++)
            for (int k = 0; k < 8; k++)
                if (ilr_wreg[q][p][k] && memcmp(&ctx->gpr[ilr_wreg[q][p][k]], &w[k], 16)) return 0;
    const uint32_t cnt0 = ldq(ls, r1 + 0x30)._u32[0], R = ldq(ls, r1 + 0x50)._u32[0], G = ldq(ls, r1 + 0x40)._u32[0];
    const uint32_t rows = R - cnt0;   /* the loop runs until the counter, incremented after each row, equals R */
    if (rows == 0 || rows > 1024 || G > 1024) return 0;
    const uint32_t centre = ldq(ls, r1 + 0x60)._u32[0], out = ctx->gpr[49]._u32[0];
    const __m128i lox = qv(ctx->gpr[55]), hix = qv(ctx->gpr[56]), loy = qv(ctx->gpr[60]), hiy = qv(ctx->gpr[61]);
    const __m128i m57 = s16x(qv(ctx->gpr[57])), m58 = s16x(qv(ctx->gpr[58]));
    /* the taps' reach: a over the clamp ranges (corners), then the quadwords read */
    int64_t amin = INT64_MAX, amax = INT64_MIN;
    for (int j = 0; j < 4; j++) {
        const int32_t ys[2] = { (int32_t)ctx->gpr[60]._u32[j], (int32_t)ctx->gpr[61]._u32[j] };
        const int32_t xs[2] = { (int32_t)ctx->gpr[55]._u32[j], (int32_t)ctx->gpr[56]._u32[j] };
        for (int a = 0; a < 2; a++)
            for (int b = 0; b < 2; b++) {
                const int64_t v = (int64_t)(int16_t)ys[a] * (int16_t)ctx->gpr[58]._u32[j] +
                                  (int64_t)(int16_t)xs[b] * (int16_t)ctx->gpr[57]._u32[j];
                amin = v < amin ? v : amin;
                amax = v > amax ? v : amax;
            }
        if (ys[0] > ys[1] || xs[0] > xs[1]) return 0;   /* a low bound above the high one: not the game's */
    }
    const int64_t s0 = ((int64_t)centre + amin) & ~15ll, s1 = (((int64_t)centre + amax) & ~15ll) + 16;
    const int64_t on = 80 * (int64_t)G * rows;
    if (s0 < 0 || s1 > SPU_LS_SIZE || (int64_t)out + on > SPU_LS_SIZE || (out & 15)) return 0;
    if (ovl(out, on, s0, s1 - s0) || ovl(out, on, r1, 880) || ovl(out, on, ILR_POOL_LO, ILR_POOL_HI - ILR_POOL_LO))
        return 0;

    __m128 X = F(ctx->gpr[36]), Y = F(ctx->gpr[62]);
    const __m128 step = F(ctx->gpr[71]), m2 = fbits(0xC0000000u), dyst = F(ldq(ls, r1 + 0x20));
    __m128 SX[8], SY[8];
    for (int k = 0; k < 8; k++) {
        SX[k] = F(ctx->gpr[63 + k]);
        SY[k] = F(ctx->gpr[75 + k]);
    }
    const uint8_t* cq = ls;
    uint32_t o = out;
    for (uint32_t r = 0; r < rows; r++) {
        __m128i dyr[8];
        for (int k = 0; k < 8; k++) {
            __m128i dy = cflts_(fm_(Y, SY[k]));
            dy = _mm_blendv_epi8(dy, hiy, _mm_cmpgt_epi32(dy, hiy));
            dy = _mm_blendv_epi8(dy, loy, _mm_cmpgt_epi32(loy, dy));
            dyr[k] = _mm_mullo_epi32(s16x(dy), m58);
        }
        for (uint32_t it = 0; it < G; it++) {
            for (int q = 0; q < 5; q++, o += 16) {
                __m128i S[4] = { _mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128() };
                for (int k = 0; k < 8; k++) {
                    __m128i dx = cflts_(fm_(X, SX[k]));
                    dx = _mm_blendv_epi8(dx, hix, _mm_cmpgt_epi32(dx, hix));
                    dx = _mm_blendv_epi8(dx, lox, _mm_cmpgt_epi32(lox, dx));
                    const __m128i a = _mm_add_epi32(dyr[k], _mm_mullo_epi32(s16x(dx), m57));
                    uint32_t av[4];
                    _mm_storeu_si128((__m128i*)av, a);
                    for (int j = 0; j < 4; j++) {
                        const uint8_t* qq = cq + LSQ(centre + av[j]);
                        const uint32_t sh = av[j] & 15;
                        const __m128i px = _mm_set_epi32(qq[(sh + 3) & 15], qq[(sh + 2) & 15], qq[(sh + 1) & 15], qq[sh]);
                        S[j] = _mm_add_epi32(S[j], _mm_mullo_epi32(px, s16x(w[k])));
                    }
                }
                uint8_t ob[16];
                for (int j = 0; j < 4; j++) {
                    uint32_t sv[4];
                    _mm_storeu_si128((__m128i*)sv, S[j]);
                    for (int c = 0; c < 4; c++) ob[4 * j + c] = (uint8_t)((sv[c] > 0xFFFFu ? 0xFFFFu : sv[c]) >> 8);
                }
                memcpy(ls + o, ob, 16);
                X = fa_(X, step);
            }
        }
        X = fa_(X, m2);
        Y = fa_(Y, dyst);
    }
    /* the row counter as the SPU leaves it (every lane + rows), then the epilogue */
    u128 cnt = ldq(ls, r1 + 0x30);
    for (int k = 0; k < 4; k++) cnt._u32[k] += rows;
    stq(ls, r1 + 0x30, cnt);
    for (int k = 0; k < 4; k++) ctx->gpr[1]._u32[k] += 880;
    const uint32_t r1n = r1 + 880;
    for (int k = 80; k <= 127; k++) ctx->gpr[k] = ldq(ls, r1n - 16 * (uint32_t)(k - 79));
    return 1;
}


/* Gates (re/spu/native_gates.tsv slots 66-74; pool hashes as post_kernels.py computes them: FNV-1a 64 over the 1.29
 * image's little-endian 8-byte words of the range its lqr loads read). */
const kz2_native_entry kz2_native_postjobs[] = {
    { 66, "KZ2_NATIVE_JOB_MODULATE", "Gfx.Post.Modulate", "job_00d56000", 0x4A20u, modulate_4A20, "modulate",
      MOD_POOL_LO, MOD_POOL_HI, 0x25ec740d5bc650abull, 0 },
    { 67, "KZ2_NATIVE_JOB_BLOOMINT", "Gfx.Post.BloomIntegrate", "job_00d62e80", 0x4A20u, bloomint_4A20, "bloomint",
      BINT_POOL_LO, BINT_POOL_HI, 0x830171fc48d67ddeull, 0 },
    { 69, "KZ2_NATIVE_JOB_VIGNETTE", "Gfx.Post.Vignette", "job_00d68f00", 0x4A20u, vignette_4A20, "vignette",
      VIG_POOL_LO, VIG_POOL_HI, 0x8918a562784103d2ull, 0 },
    { 70, "KZ2_NATIVE_JOB_DEPTHTOFUZZY", "Gfx.Post.DepthToFuzzy", "job_00d6fe00", 0x4A20u, depthfuzzy_4A20,
      "depthfuzzy", 0, 0, 0, 0 },
    { 71, "KZ2_NATIVE_JOB_DOWNSAMPLE", "Gfx.Post.Downsample", "job_00d50280", 0x5620u, down4_5620, "down4",
      DS4_POOL_LO, DS4_POOL_HI, 0x016b0478d0c8dc7dull, 0 },
    { 72, "KZ2_NATIVE_JOB_DOWNSAMPLE", "Gfx.Post.Downsample", "job_00d50280", 0x56D0u, down8_56D0, "down8",
      DS8_POOL_LO, DS8_POOL_HI, 0x5f3e096af9447e1aull, 0 },
    { 68, "KZ2_NATIVE_JOB_ILR", "Gfx.Post.ILR", "job_00d65780", 0x4E20u, ilr_4E20, "ilr", ILR_POOL_LO, ILR_POOL_HI,
      0x29c7831a6b89a765ull, 0 },
    { 73, "KZ2_NATIVE_JOB_ZERO", "Gfx.Post.Zero", "job_00d4c580", 0x5698u, zero_memset_5698, "zero", 0, 0, 0, 0 },
};
const unsigned kz2_native_postjobs_n = sizeof kz2_native_postjobs / sizeof kz2_native_postjobs[0];
