/* From Project Pyrrhus by undecuple (https://github.com/undecuple/project-pyrrhus, port/kz2/spu/native_bloom.c at 218f660),
 * MIT License, Copyright (c) 2026 undecuple -- see THIRD_PARTY_NOTICES.md. Tokisaki changes are marked "tokisaki:". */
/* native_bloom.c -- Gfx.Post.BloomCapture's pass function as native host code.
 * Default off: KZ2_NATIVE_JOB_BLOOM=1 (this and Gfx.Post.GrainWeight's pass, below), or KZ2_NATIVE_JOB_POST=1 (every
 * Gfx.Post gate).
 *
 * @0x4AD8 in job_00d5fc00 (called once per job from the body @0x43E8): the bright-pass of the bloom. It builds a
 * 129-entry curve on its stack frame, then maps every source pixel through it (the RE notes "An earlier change", from
 * tools/spu/spu_symtrace.py over the whole pass). Arguments: r3 = parameters (the quadword at r3 + 0x10: byte 4 =
 * width, byte 5 = rows, bytes 6-7 = output format: 4 = RGBA8, else 8-byte pixels of 16-bit channels), r4 = the curve
 * table (byte i at r4 + 4 i, i = 0..128; two floats a, b at r4 + 0x210 and r4 + 0x214), r5 = source RGBA8 pixels,
 * r6 = one float per pixel (the curve's input), r9 = output. rows x floor(width / 20) x 20 pixels, back to back.
 *
 *   curve LUT[i] = v + (v - LUT0 * 255) ... exactly: v = cuflt(T_i); e = fi(255, frest(255)); q = fm(v, e);
 *                  LUT[i] = fma(fnms(q, 255, v), e, q)                                 (v / 255, one Newton step)
 *   per pixel:     c_k = fm(cuflt(byte k), 1/255)  (k = 0..3; c_0 the first byte)
 *                  x = clamp(fma(f, 127, -0.5), 0, 127); j = floor(x); r = x - j
 *                  curve = fma(LUT[j], 1 - r, fm(LUT[j + 1], r))    (the shared routine @0x4178, "K4", also in
 *                                                                   Gfx.Post.GrainWeight)
 *                  lum = ((c_0 * 0 + c_1 * 0.3) + c_2 * 0.59) + c_3 * 0.11
 *                  k1 = clamp(fma(lum, a, b), 0, 1); k2 = clamp(fma(c_0, 4, k1), 0, 1); m = curve * k2
 *                  out_k = cfltu((s_k * m) * 255, scale) with s_0 = 1 and s_k = c_k otherwise;
 *                          scale 2^0 (RGBA8: the low byte stored) or 2^4 (16-bit channels: the low halfword)
 * Every operation is the runtime's own SPU helper (spu_fm, spu_fa, spu_fs, spu_fma, spu_fnms, spu_fcgt, spu_cuflt,
 * spu_cfltu, spu_frest, spu_fi) applied in the SPU code's order, so the native result is the lifted result bit for bit,
 * the exponent-255 and flush rules included. What changes is the layout: the SPU code computes one pixel's four
 * channels per vector and its scalar chain (lum .. m) in one lane of a vector per pixel; here four pixels share a
 * vector (one lane each) at every step, so the scalar chain costs a quarter, and nothing goes through the register
 * file in memory. The helpers are lane-wise (each result lane depends only on the same lane of the operands), so the
 * lanes can be rearranged.
 *
 * The local store is left as the SPU code leaves it: the outputs, and its stack frame (r1 lowered by 2576): the saved
 * r80-r109 at r1 - 16 (k - 79), r0 at r1 + 16, the back chain at the new r1, the curve at new r1 + 32 (129 splatted
 * quadwords), and the routine 0x4178's store of r1 at new r1 - 48 when it ran (at least one pixel). r1, r0 and
 * r80-r127 end as they began; r2-r79 are left as they were (post_kernels.py --live: the caller @0x43E8 reads none).
 * The native code declines (lifted body) on unaligned buffers, a table address not a multiple of 4, any range that
 * wraps the local store, or any overlap between what it writes (outputs, the frame) and what it reads (pixels, floats,
 * table, parameters, the job's constants).
 */
#include "native_simd.h"   /* the LS access and the helpers' fast paths (moved there from here) */

#define BLOOM_POOL_LO 0x6F80
#define BLOOM_POOL_HI 0x7050

static int bloom_4AD8(spu_context* ctx)   /* @0x4AD8 */
{
    uint8_t* ls = ctx->ls;
    const uint32_t r1 = ctx->gpr[1]._u32[0], prm = ctx->gpr[3]._u32[0], tab = ctx->gpr[4]._u32[0];
    const uint32_t src = ctx->gpr[5]._u32[0], fl = ctx->gpr[6]._u32[0], dst = ctx->gpr[9]._u32[0];
    const uint8_t* pq = ls + LSQ(prm + 0x10);
    const uint32_t width = pq[4], rows = pq[5], fmt = (uint32_t)pq[6] << 8 | pq[7];
    const uint32_t n = rows * (width / 20) * 20, P = fmt == 4 ? 4 : 8;
    if ((r1 | src | fl | dst) & 15 || tab & 3) return 0;
    /* the frame: r1 - 2576 - 48 (the routine's store) .. r1 + 32 (r0's slot) */
    const int64_t f0 = (int64_t)r1 - 2576 - 48, f1 = (int64_t)r1 + 32;
    if (f0 < 0 || f1 > SPU_LS_SIZE || (int64_t)tab + 0x218 > SPU_LS_SIZE) return 0;
    if ((int64_t)src + 4 * n > SPU_LS_SIZE || (int64_t)fl + 4 * n > SPU_LS_SIZE || (int64_t)dst + P * n > SPU_LS_SIZE)
        return 0;
    const int64_t rd[5][2] = { { src, 4 * (int64_t)n }, { fl, 4 * (int64_t)n }, { tab, 0x218 },
                               { LSQ(prm + 0x10), 16 }, { BLOOM_POOL_LO, BLOOM_POOL_HI - BLOOM_POOL_LO } };
    for (int i = 0; i < 5; i++)
        if (ovl(f0, f1 - f0, rd[i][0], rd[i][1]) || ovl(dst, (int64_t)P * n, rd[i][0], rd[i][1])) return 0;
    if (ovl(f0, f1 - f0, dst, (int64_t)P * n)) return 0;

    /* the frame */
    const uint32_t r1n = r1 - 2576;
    u128 nr1 = ctx->gpr[1];
    for (int k = 0; k < 4; k++) nr1._u32[k] -= 2576;
    for (int k = 80; k <= 109; k++) stq(ls, r1 - 16 * (uint32_t)(k - 79), ctx->gpr[k]);
    stq(ls, r1 + 16, ctx->gpr[0]);
    stq(ls, r1n, ctx->gpr[1]);
    /* the curve, four entries a vector (the helpers are lane-wise): v / 255 with one Newton step on fi(frest(255)) */
    const u128 k255 = sp(0x437F0000u), e = spu_fi(k255, spu_frest(k255));
    for (uint32_t i = 0; i <= 128; i += 4) {
        u128 t;
        for (int l = 0; l < 4; l++) t._u32[l] = ls[LSM(tab + 4 * (i + (uint32_t)l))];
        const u128 v = spu_cuflt(t, 155), q = spu_fm(v, e), r = spu_fma(spu_fnms(q, k255, v), e, q);
        for (uint32_t l = 0; l < 4 && i + l <= 128; l++) stq(ls, r1n + 32 + 16 * (i + l), sp(r._u32[l]));
    }
    if (!n) return 1;
    stq(ls, r1n - 48, nr1);   /* the routine 0x4178's `stqd $r1, -0x30($r1)` */

    const uint32_t lut = r1n + 32;
    const __m128 a = _mm_castsi128_ps(_mm_set1_epi32((int)be32(ls + tab + 0x210)));
    const __m128 b = _mm_castsi128_ps(_mm_set1_epi32((int)be32(ls + tab + 0x214)));
    const __m128 one = _mm_set1_ps(1.0f), k127 = _mm_set1_ps(127.0f), mhalf = _mm_set1_ps(-0.5f);
    const __m128 k4 = _mm_set1_ps(4.0f), f255 = _mm_set1_ps(255.0f);
    const __m128 inv255 = _mm_castsi128_ps(_mm_set1_epi32(0x3B808080));
    const __m128 w1 = _mm_castsi128_ps(_mm_set1_epi32(0x3E999999)), w2 = _mm_castsi128_ps(_mm_set1_epi32(0x3F170A3D));
    const __m128 w3 = _mm_castsi128_ps(_mm_set1_epi32(0x3DE147AE));
    const int scale = P == 4 ? 173 : 169;
    const __m128i bs = _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
    /* channel k of the four pixels of a quadword as four words (lane = pixel) */
    const __m128i ch[4] = { _mm_set_epi8(-1, -1, -1, 12, -1, -1, -1, 8, -1, -1, -1, 4, -1, -1, -1, 0),
                            _mm_set_epi8(-1, -1, -1, 13, -1, -1, -1, 9, -1, -1, -1, 5, -1, -1, -1, 1),
                            _mm_set_epi8(-1, -1, -1, 14, -1, -1, -1, 10, -1, -1, -1, 6, -1, -1, -1, 2),
                            _mm_set_epi8(-1, -1, -1, 15, -1, -1, -1, 11, -1, -1, -1, 7, -1, -1, -1, 3) };
    for (uint32_t g = 0; g < n / 4; g++) {
        const __m128i px = _mm_loadu_si128((const __m128i*)(ls + src + 16 * g));
        /* the curve for the four pixels (the routine @0x4178) */
        const __m128 f = _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + fl + 16 * g)), bs));
        const __m128 x = clamp_(fma_(f, k127, mhalf), k127);
        const __m128i j16 = cfltu_(x, 169);
        const __m128 fr = fs_(x, _mm_cvtepi32_ps(cfltu_(x, 173)));   /* cuflt of 0..127: exact */
        uint32_t jj[4];
        _mm_storeu_si128((__m128i*)jj, j16);
        const __m128 lo = _mm_castsi128_ps(_mm_set_epi32((int)be32(ls + LSQ(lut + jj[3]) + 12), (int)be32(ls + LSQ(lut + jj[2]) + 8),
                                                         (int)be32(ls + LSQ(lut + jj[1]) + 4), (int)be32(ls + LSQ(lut + jj[0]))));
        const __m128 hi = _mm_castsi128_ps(_mm_set_epi32((int)be32(ls + LSQ(lut + jj[3] + 16) + 12),
                                                         (int)be32(ls + LSQ(lut + jj[2] + 16) + 8),
                                                         (int)be32(ls + LSQ(lut + jj[1] + 16) + 4),
                                                         (int)be32(ls + LSQ(lut + jj[0] + 16))));
        const __m128 curve = fma_(lo, fs_(one, fr), fm_(hi, fr));
        __m128 c[4];
        for (int k = 0; k < 4; k++) c[k] = fm_(_mm_cvtepi32_ps(_mm_shuffle_epi8(px, ch[k])), inv255);   /* cuflt of a byte: exact */
        __m128 lum = fa_(fm_(c[0], _mm_setzero_ps()), fm_(c[1], w1));
        lum = fa_(fa_(lum, fm_(c[2], w2)), fm_(c[3], w3));
        const __m128 k1 = clamp_(fma_(lum, a, b), one);
        const __m128 k2 = clamp_(fma_(c[0], k4, k1), one);
        const __m128 m = fm_(curve, k2);
        __m128i o[4];
        o[0] = cfltu_(fm_(fm_(one, m), f255), scale);
        for (int k = 1; k < 4; k++) o[k] = cfltu_(fm_(fm_(c[k], m), f255), scale);
        if (P == 4) {
            /* low byte of each word, pixel-major */
            const __m128i b01 = _mm_or_si128(_mm_and_si128(o[0], _mm_set1_epi32(0xFF)), _mm_slli_epi32(_mm_and_si128(o[1], _mm_set1_epi32(0xFF)), 8));
            const __m128i b23 = _mm_or_si128(_mm_slli_epi32(_mm_and_si128(o[2], _mm_set1_epi32(0xFF)), 16), _mm_slli_epi32(o[3], 24));
            _mm_storeu_si128((__m128i*)(ls + dst + 16 * g), _mm_or_si128(b01, b23));
        } else {
            /* low halfword of each word, big-endian, pixel-major: 2 quadwords */
            const __m128i sw = _mm_set_epi8(-1, -1, -1, -1, -1, -1, -1, -1, 12, 13, 8, 9, 4, 5, 0, 1);
            const __m128i h01 = _mm_unpacklo_epi32(o[0], o[1]), h23 = _mm_unpacklo_epi32(o[2], o[3]);
            const __m128i h01b = _mm_unpackhi_epi32(o[0], o[1]), h23b = _mm_unpackhi_epi32(o[2], o[3]);
            /* pixel l = words o[0..3] lane l */
            const __m128i p0 = _mm_unpacklo_epi64(h01, h23), p1 = _mm_unpackhi_epi64(h01, h23);
            const __m128i p2 = _mm_unpacklo_epi64(h01b, h23b), p3 = _mm_unpackhi_epi64(h01b, h23b);
            uint8_t* out = ls + dst + 32 * g;
            _mm_storeu_si128((__m128i*)out, _mm_unpacklo_epi64(_mm_shuffle_epi8(p0, sw), _mm_shuffle_epi8(p1, sw)));
            _mm_storeu_si128((__m128i*)(out + 16), _mm_unpacklo_epi64(_mm_shuffle_epi8(p2, sw), _mm_shuffle_epi8(p3, sw)));
        }
    }
    return 1;
}


/* Gfx.Post.GrainWeight @0x4AD8 (job_00d4df80): the same frame and curve routine, a different pass. The curve is the
 * table's bytes as they are (LUT[i] = cuflt(the byte at r4 + 4 i + 3)), its input the pixel's weighted sum
 * lum = ((c_0 * 0 + c_1 w_1) + c_2 w_2) + c_3 w_3 of the raw channel values (w = 0.3/255, 0.59/255, 0.11/255 as the
 * pool's words 0x3A9A33CC, 0x3B17A1DE, 0x39E229D6), and the output pixel is the r6 pixel with its first byte replaced
 * by min(cfltu(curve(lum), 2^1), 255). Frame: r1 lowered by 2512, r80-r105 saved, otherwise as BloomCapture's. */
#define GRAIN_POOL_LO 0x6000
#define GRAIN_POOL_HI 0x60B0

static int grain_4AD8(spu_context* ctx)   /* @0x4AD8 */
{
    uint8_t* ls = ctx->ls;
    const uint32_t r1 = ctx->gpr[1]._u32[0], prm = ctx->gpr[3]._u32[0], tab = ctx->gpr[4]._u32[0];
    const uint32_t src = ctx->gpr[5]._u32[0], bg = ctx->gpr[6]._u32[0], dst = ctx->gpr[9]._u32[0];
    const uint8_t* pq = ls + LSQ(prm + 0x10);
    const uint32_t width = pq[4], rows = pq[5];
    const uint32_t n = rows * (width / 20) * 20;
    if ((r1 | src | bg | dst) & 15 || tab & 3) return 0;
    const int64_t f0 = (int64_t)r1 - 2512 - 48, f1 = (int64_t)r1 + 32;
    if (f0 < 0 || f1 > SPU_LS_SIZE || (int64_t)tab + 0x208 > SPU_LS_SIZE) return 0;
    if ((int64_t)src + 4 * n > SPU_LS_SIZE || (int64_t)bg + 4 * n > SPU_LS_SIZE || (int64_t)dst + 4 * n > SPU_LS_SIZE)
        return 0;
    const int64_t rd[5][2] = { { src, 4 * (int64_t)n }, { bg, 4 * (int64_t)n }, { tab, 0x208 },
                               { LSQ(prm + 0x10), 16 }, { GRAIN_POOL_LO, GRAIN_POOL_HI - GRAIN_POOL_LO } };
    for (int i = 0; i < 5; i++)
        if (ovl(f0, f1 - f0, rd[i][0], rd[i][1]) || ovl(dst, 4 * (int64_t)n, rd[i][0], rd[i][1])) return 0;
    if (ovl(f0, f1 - f0, dst, 4 * (int64_t)n)) return 0;

    const uint32_t r1n = r1 - 2512;
    u128 nr1 = ctx->gpr[1];
    for (int k = 0; k < 4; k++) nr1._u32[k] -= 2512;
    for (int k = 80; k <= 105; k++) stq(ls, r1 - 16 * (uint32_t)(k - 79), ctx->gpr[k]);
    stq(ls, r1 + 16, ctx->gpr[0]);
    stq(ls, r1n, ctx->gpr[1]);
    for (uint32_t i = 0; i <= 128; i++) {   /* cuflt of a byte is exact: the float's bits, splatted, big-endian */
        const float f = (float)ls[LSM(tab + 4 * i + 3)];
        uint32_t u;
        memcpy(&u, &f, 4);
        const __m128i v = _mm_set1_epi32((int)__builtin_bswap32(u));
        _mm_storeu_si128((__m128i*)(ls + LSQ(r1n + 32 + 16 * i)), v);
    }
    if (!n) return 1;
    stq(ls, r1n - 48, nr1);   /* the routine 0x4178's `stqd $r1, -0x30($r1)` */

    const uint32_t lut = r1n + 32;
    const __m128 one = _mm_set1_ps(1.0f), k127 = _mm_set1_ps(127.0f), mhalf = _mm_set1_ps(-0.5f);
    const __m128 w1 = _mm_castsi128_ps(_mm_set1_epi32(0x3A9A33CC)), w2 = _mm_castsi128_ps(_mm_set1_epi32(0x3B17A1DE));
    const __m128 w3 = _mm_castsi128_ps(_mm_set1_epi32(0x39E229D6));
    const __m128i ch[4] = { _mm_set_epi8(-1, -1, -1, 12, -1, -1, -1, 8, -1, -1, -1, 4, -1, -1, -1, 0),
                            _mm_set_epi8(-1, -1, -1, 13, -1, -1, -1, 9, -1, -1, -1, 5, -1, -1, -1, 1),
                            _mm_set_epi8(-1, -1, -1, 14, -1, -1, -1, 10, -1, -1, -1, 6, -1, -1, -1, 2),
                            _mm_set_epi8(-1, -1, -1, 15, -1, -1, -1, 11, -1, -1, -1, 7, -1, -1, -1, 3) };
    for (uint32_t g = 0; g < n / 4; g++) {
        const __m128i px = _mm_loadu_si128((const __m128i*)(ls + src + 16 * g));
        __m128 c[4];
        for (int k = 0; k < 4; k++) c[k] = _mm_cvtepi32_ps(_mm_shuffle_epi8(px, ch[k]));   /* cuflt of a byte: exact */
        __m128 lum = fa_(fm_(c[0], _mm_setzero_ps()), fm_(c[1], w1));
        lum = fa_(fa_(lum, fm_(c[2], w2)), fm_(c[3], w3));
        /* the curve (the routine @0x4178) */
        const __m128 x = clamp_(fma_(lum, k127, mhalf), k127);
        const __m128i j16 = cfltu_(x, 169);
        const __m128 fr = fs_(x, _mm_cvtepi32_ps(cfltu_(x, 173)));
        uint32_t jj[4];
        _mm_storeu_si128((__m128i*)jj, j16);
        const __m128 lo = _mm_castsi128_ps(_mm_set_epi32((int)be32(ls + LSQ(lut + jj[3]) + 12), (int)be32(ls + LSQ(lut + jj[2]) + 8),
                                                         (int)be32(ls + LSQ(lut + jj[1]) + 4), (int)be32(ls + LSQ(lut + jj[0]))));
        const __m128 hi = _mm_castsi128_ps(_mm_set_epi32((int)be32(ls + LSQ(lut + jj[3] + 16) + 12),
                                                         (int)be32(ls + LSQ(lut + jj[2] + 16) + 8),
                                                         (int)be32(ls + LSQ(lut + jj[1] + 16) + 4),
                                                         (int)be32(ls + LSQ(lut + jj[0] + 16))));
        const __m128 curve = fma_(lo, fs_(one, fr), fm_(hi, fr));
        /* min(cfltu(curve, 172), 255) as an unsigned compare (clgt + selb) */
        const __m128i v = _mm_min_epu32(cfltu_(curve, 172), _mm_set1_epi32(255));
        const __m128i b = _mm_loadu_si128((const __m128i*)(ls + bg + 16 * g));
        const __m128i m0 = _mm_set1_epi32(0xFF);   /* host bytes: byte 0 of each big-endian pixel is the low byte */
        _mm_storeu_si128((__m128i*)(ls + dst + 16 * g), _mm_or_si128(_mm_andnot_si128(m0, b), v));
    }
    return 1;
}

const kz2_native_entry kz2_native_bloom[] = {
    { 64, "KZ2_NATIVE_JOB_BLOOM", "Gfx.Post.BloomCapture", "job_00d5fc00", 0x4AD8u, bloom_4AD8, "bloom",
      BLOOM_POOL_LO, BLOOM_POOL_HI, 0x6d7488bfe30322eaull, 0 },
    { 65, "KZ2_NATIVE_JOB_BLOOM", "Gfx.Post.GrainWeight", "job_00d4df80", 0x4AD8u, grain_4AD8, "grain",
      GRAIN_POOL_LO, GRAIN_POOL_HI, 0xe1241d82e32ec75aull, 0 },
};
const unsigned kz2_native_bloom_n = sizeof kz2_native_bloom / sizeof kz2_native_bloom[0];
