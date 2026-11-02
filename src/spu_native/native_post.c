/* From Project Pyrrhus by undecuple (https://github.com/undecuple/project-pyrrhus, port/kz2/spu/native_post.c at 218f660),
 * MIT License, Copyright (c) 2026 undecuple -- see THIRD_PARTY_NOTICES.md. Tokisaki changes are marked "tokisaki:". */
/* native_post.c -- Gfx.Post kernels as native host code: Gfx.Post.Upsample's own two
 * kernels and the three output kernels every Gfx.Post job image carries (16 of the 17; Gfx.Post.Unlock has none).
 * Default off: KZ2_NATIVE_JOB_UPSAMPLE=1 (Upsample's kernels), KZ2_NATIVE_JOB_POSTMIX=1 (the shared kernels in every
 * image), KZ2_NATIVE_JOB_POST=1 (every Gfx.Post gate, DepthOfField's included).
 *
 * The shared kernels were found by tools/spu/post_kernels.py: the subroutines of the 17 images compared instruction
 * by instruction after normalising what depends on where they sit (PC-relative branch targets as offsets into the
 * subroutine, each lqr replaced by the 16 bytes it loads). Three groups are the same code in 16 images (table at the
 * end; the RE notes an earlier change): "blend" (289 instructions, an earlier change's DepthOfField @0x4CB0), "addb" (199) and
 * "addh" (187). Each image's job body calls one of them at the end of a pass to combine its result with the target
 * (r3 = target, updated in place; r4 = the pass's result; r5 = pixels, a multiple of 20).
 *
 * Every call site was checked for scratch registers read after the call (post_kernels.py --live: inter-procedural,
 * returns and callees followed): none at any of the 50 sites, so the kernels keep r0, r1, r80-r127 and leave r2-r79
 * as they were, like an earlier change's. Integer only: no SPU float rules involved. Written from the disassembly with
 * tools/spu/spu_symtrace.py and checked against the lifted code by tools/spu/native_diff.py (synthetic and recorded
 * inputs, the interpreter as a third side).
 */
#include "kz2_native_jobs.h"

#include <smmintrin.h>
#include <string.h>

#define LSQ(a) ((a) & (SPU_LS_MASK & ~0xFu))

static inline int overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return a < b + bn && b < a + an;
}

static inline int64_t i64(uint32_t v)
{
    return (int64_t)v;
}

/* @0x4CB0 in DepthOfField (and its 15 copies, table below; loop at +0xB8): r3 = RGBA8 pixels, blended in place,
 * r4 = RGBA8 pixels underneath, r5 = pixels (a multiple of 20: 5 quadwords a step; any other count never reaches
 * 0 in the SPU loop). Per pixel, with a0 the first byte (the coverage) of the r3 pixel:
 *   out_j = min(a_j + ((b_j * (256 - a0)) >> 8), 255)
 * (a premultiplied layer over the image). Integer only: mpy (signed 16 x 16; here 0..65280), rotmai 8, a, cgt + selb
 * (signed > 255). The SSE form: 16-bit lanes (the products fit), packus saturates to the same 255. */
int kz2_post_blend(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    uint32_t a = ctx->gpr[3]._u32[0], b = ctx->gpr[4]._u32[0];
    const uint32_t n = ctx->gpr[5]._u32[0];
    if (n == 0 || n % 20 || n > 16384 || overlaps(LSQ(a), 4 * n, LSQ(b), 4 * n + 80)) return 0;
    const __m128i z = _mm_setzero_si128(), k256 = _mm_set1_epi16(256);
    /* the first byte of each pixel, as a 16-bit lane under every channel of its pixel */
    const __m128i a3lo = _mm_set_epi8(-1, 4, -1, 4, -1, 4, -1, 4, -1, 0, -1, 0, -1, 0, -1, 0);
    const __m128i a3hi = _mm_set_epi8(-1, 12, -1, 12, -1, 12, -1, 12, -1, 8, -1, 8, -1, 8, -1, 8);
    for (uint32_t q = 0; q < n / 4; q++, a += 16, b += 16) {
        uint8_t* pa = ls + LSQ(a);
        const __m128i va = _mm_loadu_si128((const __m128i*)pa);
        const __m128i vb = _mm_loadu_si128((const __m128i*)(ls + LSQ(b)));
        const __m128i klo = _mm_sub_epi16(k256, _mm_shuffle_epi8(va, a3lo));
        const __m128i khi = _mm_sub_epi16(k256, _mm_shuffle_epi8(va, a3hi));
        const __m128i lo = _mm_add_epi16(_mm_unpacklo_epi8(va, z), _mm_srli_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(vb, z), klo), 8));
        const __m128i hi = _mm_add_epi16(_mm_unpackhi_epi8(va, z), _mm_srli_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(vb, z), khi), 8));
        _mm_storeu_si128((__m128i*)pa, _mm_packus_epi16(lo, hi));
    }
    return 1;
}

/* "addb" (@0x50D8 in Upsample; loop of 5 quadwords): r3 += r4 per byte, saturated: each byte zero-extended to a word
 * (shufb), a, cgt 255 + selb, the low bytes packed back. r5 = 4-byte pixels, a multiple of 20. = paddusb. */
static int post_addb(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    uint32_t a = ctx->gpr[3]._u32[0], b = ctx->gpr[4]._u32[0];
    const uint32_t n = ctx->gpr[5]._u32[0];
    if (n == 0 || n % 20 || n > 16384 || overlaps(LSQ(a), 4 * n, LSQ(b), 4 * n + 80)) return 0;
    for (uint32_t q = 0; q < n / 4; q++, a += 16, b += 16) {
        uint8_t* pa = ls + LSQ(a);
        const __m128i vb = _mm_loadu_si128((const __m128i*)(ls + LSQ(b)));
        _mm_storeu_si128((__m128i*)pa, _mm_adds_epu8(_mm_loadu_si128((const __m128i*)pa), vb));
    }
    return 1;
}

/* "addh" (@0x53F8 in Upsample): the same for 8-byte pixels of four big-endian 16-bit channels: each halfword
 * zero-extended, a, cgt 0xFFFF + selb, packed back. r5 = pixels, a multiple of 20 (10 quadwords a step). = paddusw on
 * the byte-swapped halfwords. */
static int post_addh(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    uint32_t a = ctx->gpr[3]._u32[0], b = ctx->gpr[4]._u32[0];
    const uint32_t n = ctx->gpr[5]._u32[0];
    if (n == 0 || n % 20 || n > 8192 || overlaps(LSQ(a), 8 * n, LSQ(b), 8 * n + 160)) return 0;
    const __m128i sw = _mm_set_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);
    for (uint32_t q = 0; q < n / 2; q++, a += 16, b += 16) {
        uint8_t* pa = ls + LSQ(a);
        const __m128i va = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)pa), sw);
        const __m128i vb = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + LSQ(b))), sw);
        _mm_storeu_si128((__m128i*)pa, _mm_shuffle_epi8(_mm_adds_epu16(va, vb), sw));
    }
    return 1;
}

/* Arguments of the two upsample kernels (the pass @0x4A20 calls one per pair of output rows): r3 = output row A,
 * r4 = source row 0 (rows 1 and 2 follow at r5 and 2 r5), r5 = source row stride, r6 = output row stride (row B =
 * r3 + r6), r7 = output pixels per row (8 a step: a multiple of 8; 0 never ends the SPU loop). P = bytes per pixel.
 * The native code wants what the game passes: 16-byte aligned rows and strides, nothing wrapping the local store,
 * the two output rows apart from each other and from every source byte read (the quadword before and after each
 * source row included). Otherwise the lifted code runs. */
static int up_args_ok(spu_context* ctx, uint32_t P)
{
    const uint32_t d = ctx->gpr[3]._u32[0], s = ctx->gpr[4]._u32[0], ss = ctx->gpr[5]._u32[0];
    const uint32_t ds = ctx->gpr[6]._u32[0], n = ctx->gpr[7]._u32[0];
    if (n == 0 || n % 8 || n > 4096 || ((d | s | ss | ds) & 15)) return 0;
    const int64_t ob = i64(n) * P, ib = i64(n / 2) * P;
    const int64_t r0 = i64(s) - 16, r2end = i64(s) + 2 * i64(ss) + ib + 16;
    if (r0 < 0 || r2end > SPU_LS_SIZE || i64(d) + i64(ds) + ob > SPU_LS_SIZE || ds < ob) return 0;
    for (int k = 0; k < 3; k++) {
        const int64_t lo = i64(s) + k * i64(ss) - 16, len = ib + 32;
        if (overlaps(d, (uint32_t)ob, (uint32_t)lo, (uint32_t)len) || overlaps(d + ds, (uint32_t)ob, (uint32_t)lo, (uint32_t)len))
            return 0;
    }
    return 1;
}

/* @0x56F0 (loop 0x5778, 4 source pixels a step): 2x upsampling of RGBA8 (Gfx.Post.Upsample's 4-byte path). For source
 * pixel x of the middle row (row 1) and each channel, with the row's neighbours x-1 and x+1 (the bytes just before
 * and after the pixel, also at the row ends):
 *   hL(row) = (3 row[x] + row[x-1]) >> 2      hR(row) = (3 row[x] + row[x+1]) >> 2
 *   row A: out[2x] = (3 hL(1) + hL(0)) >> 2   out[2x+1] = (3 hR(1) + hR(0)) >> 2
 *   row B: out[2x] = (3 hL(1) + hL(2)) >> 2   out[2x+1] = (3 hR(1) + hR(2)) >> 2
 * (mpya by 3 plus the neighbour, rotmi 2: a 3/4-1/4 bilinear tap, truncated after each pass; the low bytes stored).
 * Every value is at most 1020, so 16-bit lanes hold them exactly. */
static int up4_56F0(spu_context* ctx)
{
    if (!up_args_ok(ctx, 4)) return 0;
    uint8_t* ls = ctx->ls;
    const uint32_t d = ctx->gpr[3]._u32[0], s = ctx->gpr[4]._u32[0], ss = ctx->gpr[5]._u32[0];
    const uint32_t ds = ctx->gpr[6]._u32[0], nq = ctx->gpr[7]._u32[0] / 8;
    const __m128i z = _mm_setzero_si128();
    for (uint32_t q = 0; q < nq; q++) {
        __m128i hl[3][2], hr[3][2];
        for (int r = 0; r < 3; r++) {
            const uint8_t* p = ls + s + r * ss + 16 * q;
            const __m128i c = _mm_loadu_si128((const __m128i*)p);
            const __m128i l = _mm_alignr_epi8(c, _mm_loadu_si128((const __m128i*)(p - 16)), 12);
            const __m128i rr = _mm_alignr_epi8(_mm_loadu_si128((const __m128i*)(p + 16)), c, 4);
            const __m128i cl = _mm_unpacklo_epi8(c, z), ch = _mm_unpackhi_epi8(c, z);
            const __m128i c3l = _mm_add_epi16(_mm_add_epi16(cl, cl), cl), c3h = _mm_add_epi16(_mm_add_epi16(ch, ch), ch);
            hl[r][0] = _mm_srli_epi16(_mm_add_epi16(c3l, _mm_unpacklo_epi8(l, z)), 2);
            hl[r][1] = _mm_srli_epi16(_mm_add_epi16(c3h, _mm_unpackhi_epi8(l, z)), 2);
            hr[r][0] = _mm_srli_epi16(_mm_add_epi16(c3l, _mm_unpacklo_epi8(rr, z)), 2);
            hr[r][1] = _mm_srli_epi16(_mm_add_epi16(c3h, _mm_unpackhi_epi8(rr, z)), 2);
        }
        for (int row = 0; row < 2; row++) {
            const int o = row ? 2 : 0;
            __m128i ev[2], od[2];
            for (int h = 0; h < 2; h++) {
                const __m128i l1 = hl[1][h], r1 = hr[1][h];
                ev[h] = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_add_epi16(l1, l1), l1), hl[o][h]), 2);
                od[h] = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_add_epi16(r1, r1), r1), hr[o][h]), 2);
            }
            const __m128i e = _mm_packus_epi16(ev[0], ev[1]), f = _mm_packus_epi16(od[0], od[1]);
            uint8_t* out = ls + d + row * ds + 32 * q;
            _mm_storeu_si128((__m128i*)out, _mm_unpacklo_epi32(e, f));
            _mm_storeu_si128((__m128i*)(out + 16), _mm_unpackhi_epi32(e, f));
        }
    }
    return 1;
}

/* One 8-byte pixel (four big-endian 16-bit channels) as four 32-bit lanes, zero-extended (the SPU code's shufb). */
static inline __m128i px8(const uint8_t* p)
{
    return _mm_shuffle_epi8(_mm_loadl_epi64((const __m128i*)p),
                            _mm_set_epi8(-1, -1, 6, 7, -1, -1, 4, 5, -1, -1, 2, 3, -1, -1, 0, 1));
}

/* mpya x, 3, y then rotmi 2: the signed 16-bit low half of x times 3 plus the whole word y, shifted right logically
 * (mod 2^32: a channel of 0x8000 or more counts as negative in the product, as on the SPU). */
static inline __m128i tap3(__m128i x, __m128i y)
{
    const __m128i sx = _mm_srai_epi32(_mm_slli_epi32(x, 16), 16);
    return _mm_srli_epi32(_mm_add_epi32(_mm_add_epi32(_mm_add_epi32(sx, sx), sx), y), 2);
}

/* @0x5A20 (loop 0x5AE0, 4 source pixels a step): the same upsampling for 8-byte pixels of 16-bit channels
 * (Gfx.Post.Upsample's other path): the formulas of @0x56F0 in 32-bit lanes, with the SPU's mpya semantics
 * (tap3); the low halfword of each result stored. */
static int up8_5A20(spu_context* ctx)
{
    if (!up_args_ok(ctx, 8)) return 0;
    uint8_t* ls = ctx->ls;
    const uint32_t d = ctx->gpr[3]._u32[0], s = ctx->gpr[4]._u32[0], ss = ctx->gpr[5]._u32[0];
    const uint32_t ds = ctx->gpr[6]._u32[0], np = ctx->gpr[7]._u32[0] / 2;
    const __m128i pk = _mm_set_epi8(-1, -1, -1, -1, -1, -1, -1, -1, 12, 13, 8, 9, 4, 5, 0, 1);
    for (uint32_t x = 0; x < np; x++) {
        __m128i hl[3], hr[3];
        for (int r = 0; r < 3; r++) {
            const uint8_t* p = ls + s + r * ss + 8 * x;
            const __m128i c = px8(p);
            hl[r] = tap3(c, px8(p - 8));
            hr[r] = tap3(c, px8(p + 8));
        }
        for (int row = 0; row < 2; row++) {
            const int o = row ? 2 : 0;
            uint8_t* out = ls + d + row * ds + 16 * x;
            _mm_storel_epi64((__m128i*)out, _mm_shuffle_epi8(tap3(hl[1], hl[o]), pk));
            _mm_storel_epi64((__m128i*)(out + 8), _mm_shuffle_epi8(tap3(hr[1], hr[o]), pk));
        }
    }
    return 1;
}

/* Gates (re/spu/native_gates.tsv; rows and pools from tools/spu/post_kernels.py --table): slot, flags, job, image,
 * LS address, kernel, family, constant pool [lo, hi) and its hash. DepthOfField's blend is slot 2 (native_dof.c). */
#define UP_ENV "KZ2_NATIVE_JOB_UPSAMPLE"
#define MIX_ENV "KZ2_NATIVE_JOB_POSTMIX"
const kz2_native_entry kz2_native_post[] = {
    { 3, UP_ENV, "Gfx.Post.Upsample", "job_00d51f00", 0x56F0u, up4_56F0, "upsample4", 0x5F80u, 0x6010u, 0x7ebb5d25c354924full, 0 },
    { 4, UP_ENV, "Gfx.Post.Upsample", "job_00d51f00", 0x5A20u, up8_5A20, "upsample8", 0x5FE0u, 0x6020u, 0x5f3e096af9447e1aull, 0 },
    { 5, MIX_ENV, "Gfx.Post.Zero", "job_00d4c580", 0x4A60u, kz2_post_blend, "blend", 0x57D0u, 0x5860u, 0x7ebb5d25c354924full, 0 },
    { 6, MIX_ENV, "Gfx.Post.GrainWeight", "job_00d4df80", 0x5380u, kz2_post_blend, "blend", 0x60B0u, 0x6140u, 0x7ebb5d25c354924full, 0 },
    { 7, MIX_ENV, "Gfx.Post.Downsample", "job_00d50280", 0x4B80u, kz2_post_blend, "blend", 0x59C0u, 0x5A50u, 0x7ebb5d25c354924full, 0 },
    { 8, MIX_ENV, "Gfx.Post.Upsample", "job_00d51f00", 0x4C50u, kz2_post_blend, "blend", 0x5F80u, 0x6010u, 0x7ebb5d25c354924full, 0 },
    { 9, MIX_ENV, "Gfx.Post.Add", "job_00d54100", 0x4FD0u, kz2_post_blend, "blend", 0x5CD0u, 0x5D60u, 0x7ebb5d25c354924full, 0 },
    { 10, MIX_ENV, "Gfx.Post.Modulate", "job_00d56000", 0x4F70u, kz2_post_blend, "blend", 0x5C90u, 0x5D20u, 0x7ebb5d25c354924full, 0 },
    { 11, MIX_ENV, "Gfx.Post.Blend", "job_00d57f00", 0x4EC0u, kz2_post_blend, "blend", 0x5BB0u, 0x5C40u, 0x7ebb5d25c354924full, 0 },
    { 12, MIX_ENV, "Gfx.Post.HBlur", "job_00d59d00", 0x4D60u, kz2_post_blend, "blend", 0x6F30u, 0x6FC0u, 0x7ebb5d25c354924full, 0 },
    { 13, MIX_ENV, "Gfx.Post.VBlur", "job_00d5ce80", 0x4D00u, kz2_post_blend, "blend", 0x6B10u, 0x6BA0u, 0x7ebb5d25c354924full, 0 },
    { 14, MIX_ENV, "Gfx.Post.BloomCapture", "job_00d5fc00", 0x6300u, kz2_post_blend, "blend", 0x7050u, 0x70E0u, 0x7ebb5d25c354924full, 0 },
    { 15, MIX_ENV, "Gfx.Post.BloomIntegrate", "job_00d62e80", 0x5940u, kz2_post_blend, "blend", 0x6680u, 0x6710u, 0x7ebb5d25c354924full, 0 },
    { 16, MIX_ENV, "Gfx.Post.ILR", "job_00d65780", 0x6780u, kz2_post_blend, "blend", 0x7500u, 0x7590u, 0x7ebb5d25c354924full, 0 },
    { 17, MIX_ENV, "Gfx.Post.Vignette", "job_00d68f00", 0x4C10u, kz2_post_blend, "blend", 0x58F0u, 0x5980u, 0x7ebb5d25c354924full, 0 },
    { 18, MIX_ENV, "Gfx.Post.MotionBlur", "job_00d6aa80", 0x4C40u, kz2_post_blend, "blend", 0x61B0u, 0x6240u, 0x7ebb5d25c354924full, 0 },
    { 19, MIX_ENV, "Gfx.Post.DepthToFuzzy", "job_00d6fe00", 0x4C40u, kz2_post_blend, "blend", 0x58C0u, 0x5950u, 0x7ebb5d25c354924full, 0 },
    { 20, MIX_ENV, "Gfx.Post.Zero", "job_00d4c580", 0x4EE8u, post_addb, "addb", 0x57D0u, 0x5860u, 0x7ebb5d25c354924full, 0 },
    { 21, MIX_ENV, "Gfx.Post.GrainWeight", "job_00d4df80", 0x5808u, post_addb, "addb", 0x60B0u, 0x6140u, 0x7ebb5d25c354924full, 0 },
    { 22, MIX_ENV, "Gfx.Post.Downsample", "job_00d50280", 0x5008u, post_addb, "addb", 0x59C0u, 0x5A50u, 0x7ebb5d25c354924full, 0 },
    { 23, MIX_ENV, "Gfx.Post.Upsample", "job_00d51f00", 0x50D8u, post_addb, "addb", 0x5F80u, 0x6010u, 0x7ebb5d25c354924full, 0 },
    { 24, MIX_ENV, "Gfx.Post.Add", "job_00d54100", 0x5458u, post_addb, "addb", 0x5CD0u, 0x5D60u, 0x7ebb5d25c354924full, 0 },
    { 25, MIX_ENV, "Gfx.Post.Modulate", "job_00d56000", 0x53F8u, post_addb, "addb", 0x5C90u, 0x5D20u, 0x7ebb5d25c354924full, 0 },
    { 26, MIX_ENV, "Gfx.Post.Blend", "job_00d57f00", 0x5348u, post_addb, "addb", 0x5BB0u, 0x5C40u, 0x7ebb5d25c354924full, 0 },
    { 27, MIX_ENV, "Gfx.Post.HBlur", "job_00d59d00", 0x51E8u, post_addb, "addb", 0x6F30u, 0x6FC0u, 0x7ebb5d25c354924full, 0 },
    { 28, MIX_ENV, "Gfx.Post.VBlur", "job_00d5ce80", 0x5188u, post_addb, "addb", 0x6B10u, 0x6BA0u, 0x7ebb5d25c354924full, 0 },
    { 29, MIX_ENV, "Gfx.Post.BloomCapture", "job_00d5fc00", 0x6788u, post_addb, "addb", 0x7050u, 0x70E0u, 0x7ebb5d25c354924full, 0 },
    { 30, MIX_ENV, "Gfx.Post.BloomIntegrate", "job_00d62e80", 0x5DC8u, post_addb, "addb", 0x6680u, 0x6710u, 0x7ebb5d25c354924full, 0 },
    { 31, MIX_ENV, "Gfx.Post.ILR", "job_00d65780", 0x6C08u, post_addb, "addb", 0x7500u, 0x7590u, 0x7ebb5d25c354924full, 0 },
    { 32, MIX_ENV, "Gfx.Post.Vignette", "job_00d68f00", 0x5098u, post_addb, "addb", 0x58F0u, 0x5980u, 0x7ebb5d25c354924full, 0 },
    { 33, MIX_ENV, "Gfx.Post.MotionBlur", "job_00d6aa80", 0x50C8u, post_addb, "addb", 0x61B0u, 0x6240u, 0x7ebb5d25c354924full, 0 },
    { 34, MIX_ENV, "Gfx.Post.DepthOfField", "job_00d6d100", 0x5138u, post_addb, "addb", 0x6550u, 0x6620u, 0xb65df4abc0d86d6full, 0 },
    { 35, MIX_ENV, "Gfx.Post.DepthToFuzzy", "job_00d6fe00", 0x50C8u, post_addb, "addb", 0x58C0u, 0x5950u, 0x7ebb5d25c354924full, 0 },
    { 36, MIX_ENV, "Gfx.Post.Zero", "job_00d4c580", 0x5208u, post_addh, "addh", 0x5830u, 0x5870u, 0x5f3e096af9447e1aull, 0 },
    { 37, MIX_ENV, "Gfx.Post.GrainWeight", "job_00d4df80", 0x5B28u, post_addh, "addh", 0x6110u, 0x6150u, 0x5f3e096af9447e1aull, 0 },
    { 38, MIX_ENV, "Gfx.Post.Downsample", "job_00d50280", 0x5328u, post_addh, "addh", 0x5A20u, 0x5A60u, 0x5f3e096af9447e1aull, 0 },
    { 39, MIX_ENV, "Gfx.Post.Upsample", "job_00d51f00", 0x53F8u, post_addh, "addh", 0x5FE0u, 0x6020u, 0x5f3e096af9447e1aull, 0 },
    { 40, MIX_ENV, "Gfx.Post.Add", "job_00d54100", 0x5778u, post_addh, "addh", 0x5D30u, 0x5D70u, 0x5f3e096af9447e1aull, 0 },
    { 41, MIX_ENV, "Gfx.Post.Modulate", "job_00d56000", 0x5718u, post_addh, "addh", 0x5CF0u, 0x5D30u, 0x5f3e096af9447e1aull, 0 },
    { 42, MIX_ENV, "Gfx.Post.Blend", "job_00d57f00", 0x5668u, post_addh, "addh", 0x5C10u, 0x5C50u, 0x5f3e096af9447e1aull, 0 },
    { 43, MIX_ENV, "Gfx.Post.HBlur", "job_00d59d00", 0x5508u, post_addh, "addh", 0x6F90u, 0x6FD0u, 0x5f3e096af9447e1aull, 0 },
    { 44, MIX_ENV, "Gfx.Post.VBlur", "job_00d5ce80", 0x54A8u, post_addh, "addh", 0x6B70u, 0x6BB0u, 0x5f3e096af9447e1aull, 0 },
    { 45, MIX_ENV, "Gfx.Post.BloomCapture", "job_00d5fc00", 0x6AA8u, post_addh, "addh", 0x70B0u, 0x70F0u, 0x5f3e096af9447e1aull, 0 },
    { 46, MIX_ENV, "Gfx.Post.BloomIntegrate", "job_00d62e80", 0x60E8u, post_addh, "addh", 0x66E0u, 0x6720u, 0x5f3e096af9447e1aull, 0 },
    { 47, MIX_ENV, "Gfx.Post.ILR", "job_00d65780", 0x6F28u, post_addh, "addh", 0x7560u, 0x75A0u, 0x5f3e096af9447e1aull, 0 },
    { 48, MIX_ENV, "Gfx.Post.Vignette", "job_00d68f00", 0x53B8u, post_addh, "addh", 0x5950u, 0x5990u, 0x5f3e096af9447e1aull, 0 },
    { 49, MIX_ENV, "Gfx.Post.MotionBlur", "job_00d6aa80", 0x53E8u, post_addh, "addh", 0x6210u, 0x6250u, 0x5f3e096af9447e1aull, 0 },
    { 50, MIX_ENV, "Gfx.Post.DepthOfField", "job_00d6d100", 0x5458u, post_addh, "addh", 0x65B0u, 0x6630u, 0x565e8da6533c2f5aull, 0 },
    { 51, MIX_ENV, "Gfx.Post.DepthToFuzzy", "job_00d6fe00", 0x53E8u, post_addh, "addh", 0x5920u, 0x5960u, 0x5f3e096af9447e1aull, 0 },
};
const unsigned kz2_native_post_n = sizeof kz2_native_post / sizeof kz2_native_post[0];
