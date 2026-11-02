/* From Project Pyrrhus by undecuple (https://github.com/undecuple/project-pyrrhus, port/kz2/spu/native_dof.c at 218f660),
 * MIT License, Copyright (c) 2026 undecuple -- see THIRD_PARTY_NOTICES.md. Tokisaki changes are marked "tokisaki:". */
/* native_dof.c -- Gfx.Post.DepthOfField (PostDepthOfFieldSPU, KZ2 1.29 job module at EBOOT file offset 0xD6D100,
 * image 220) kernels as native host code. Default off: KZ2_NATIVE_JOB_DOF=1.
 *
 * Written from the lifted reference and the disassembly (tools/spu/spu_symtrace.py gives the per-pixel dataflow of
 * the software-pipelined loops); bit-exact with the lifted code by construction: every float operation is the
 * runtime's own SPU helper (spu_fm, spu_fa, spu_fma, spu_frest, spu_fi, spu_cuflt, spu_cfltu, spu_fcgt: the same
 * rounding, flush and extended-range handling as the lifted job), integer work is exact, and every local-store
 * access masks its address as the SPU does (& 0x3FFF0). Checked against the lifted code on synthetic and recorded
 * inputs by tools/spu/native_diff.py.
 *
 * The job's pass function (@0x4A20) runs, for each band of up to 15 rows, the unpack kernel @0x5F80 over the band and
 * a 14-row apron into a float buffer at LS 0x6CC0, then the gather kernel @0x5750 once per row. Both kernels are
 * leaves without channel access; the gates replace them whole. They keep r0, r1 and r80-r127 (the SPU ABI's
 * preserved registers, all their caller reads afterwards); the scratch registers r2-r79 are left as they were.
 */
#include "kz2_native_jobs.h"
#include "spu_helpers.h"

#include <smmintrin.h>
#include <stdlib.h>
#include <string.h>

#define LSQ(a) ((a) & (SPU_LS_MASK & ~0xFu))

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

static inline u128 splat(uint32_t v)
{
    return spu_splat_u32(v);
}

/* The job's constant area (shuffle masks, the tap tables at 0x6630/0x6870, the lane offsets at 0x6AC0) as loaded
 * from the 1.29 image: the gather and the unpack hard-code what these constants select, so their table entries carry
 * the pool LS 0x6550-0x6B5F and its hash; kz2_native_run (native_common.c) checks it (cached per local store and
 * job header) before either kernel runs, and a different image (or a job that rewrote them) runs the lifted code. */
#define DOF_CONST_LO 0x6550u
#define DOF_CONST_HI 0x6B60u
#define DOF_CONST_HASH 0x697d75ee76f7f26bull

static inline int overlaps(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return a < b + bn && b < a + an;
}

#define K_255 0x3F808081u   /* 256/255 (1.0039216): x/256 * 256/255 = x/255 */

static inline __m128 f4(uint32_t bits)
{
    return _mm_castsi128_ps(_mm_set1_epi32((int)bits));
}

/* SPU word lanes (big-endian in the local store) <-> host lanes. */
static inline __m128i bswap32x4(__m128i v)
{
    return _mm_shuffle_epi8(v, _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3));
}

/* The fast paths below use plain SSE single-precision operations, the same instructions the runtime's helpers
 * use for fa/fm (MXCSR as set for the SPU thread), and drop to the helper itself wherever a helper would take its
 * slow path: a result lane with exponent 255 (the SPU's extended range). Integer-to-float conversions are exact for
 * the values involved (below 2^24), and scaling by a power of two is exact, so they equal spu_cuflt. */
static inline int has_exp255(__m128 r)
{
    const __m128i e = _mm_and_si128(_mm_castps_si128(r), _mm_set1_epi32(0x7F800000));
    return _mm_movemask_epi8(_mm_cmpeq_epi32(e, _mm_set1_epi32(0x7F800000))) != 0;
}

static inline u128 to_u(__m128 x)
{
    u128 r;
    memcpy(&r, &x, 16);
    return r;
}

static inline __m128 to_f(u128 x)
{
    __m128 r;
    memcpy(&r, &x, 16);
    return r;
}

static inline __m128 lane_splat(__m128 v, int p)
{
    switch (p & 3) {
    case 0: return _mm_shuffle_ps(v, v, 0x00);
    case 1: return _mm_shuffle_ps(v, v, 0x55);
    case 2: return _mm_shuffle_ps(v, v, 0xAA);
    default: return _mm_shuffle_ps(v, v, 0xFF);
    }
}

static inline __m128 fm_x(__m128 a, __m128 b)   /* spu_fm */
{
    const __m128 r = _mm_mul_ps(a, b);
    return has_exp255(r) ? to_f(spu_fm(to_u(a), to_u(b))) : r;
}

/* @0x5F80 (loops 0x6050/0x6230): r3 = float4 destination, r4 = pixels (4 bytes each: M, c1, c2, c3), r5 = weights
 * (the top byte of each word, one per pixel), r6 = groups of 16 pixels. Per pixel:
 *   x = {255, c1, c2, c3};  v = x + ((M * x) >> 4);  out = (v/256 * 256/255) * (w/256 * 256/255)
 * i.e. an RGBM-style decode (the coverage channel first) times the pixel's weight, in the SPU's own order of
 * operations (cuflt 147, fm 256/255, fm). v <= 4334 and every product is a small finite positive value, so no lane
 * can reach exponent 255 and the plain SSE operations are the helpers' results. */
/* One group of 16 pixels as the kernel computes it, host-order float4 per pixel (shared with the pass). */
static inline void unpack16(const uint8_t* ls, uint32_t src, uint32_t wts, __m128 out[16])
{
    const __m128 k = f4(K_255), s8 = f4(0x3B800000u);   /* 2^-8 */
    const __m128i c255 = _mm_set_epi32(0, 0, 0, 255);
    for (int q = 0; q < 4; q++) {
        /* raw big-endian quads: pixel p is bytes 4p..4p+3 = M, c1, c2, c3; its weight is byte 4p of b */
        const __m128i a = _mm_loadu_si128((const __m128i*)(ls + LSQ(src + 16 * q)));
        const __m128i b = _mm_loadu_si128((const __m128i*)(ls + LSQ(wts + 16 * q)));
        const __m128i wb = _mm_shuffle_epi8(b, _mm_set_epi8(-1, -1, -1, 12, -1, -1, -1, 8, -1, -1, -1, 4, -1, -1, -1, 0));
        const __m128 w = _mm_mul_ps(_mm_mul_ps(_mm_cvtepi32_ps(wb), s8), k);
        for (int p = 0; p < 4; p++) {
            const char o = (char)(4 * p);
            const __m128i x = _mm_or_si128(c255, _mm_shuffle_epi8(a, _mm_set_epi8(-1, -1, -1, (char)(o + 3), -1, -1, -1,
                                                                                   (char)(o + 2), -1, -1, -1, (char)(o + 1),
                                                                                   -1, -1, -1, -1)));
            const __m128i m = _mm_shuffle_epi8(a, _mm_set_epi8(-1, -1, -1, o, -1, -1, -1, o, -1, -1, -1, o, -1, -1, -1, o));
            const __m128i v = _mm_add_epi32(x, _mm_srli_epi32(_mm_mullo_epi16(m, x), 4));
            const __m128 f = _mm_mul_ps(_mm_mul_ps(_mm_cvtepi32_ps(v), s8), k);
            out[4 * q + p] = _mm_mul_ps(f, lane_splat(w, p));
        }
    }
}

static int dof_unpack_5F80(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    uint32_t dst = ctx->gpr[3]._u32[0], src = ctx->gpr[4]._u32[0], wts = ctx->gpr[5]._u32[0];
    uint32_t n = ctx->gpr[6]._u32[0];
    if (n == 0 || n > 64 || overlaps(LSQ(dst), 256 * n, LSQ(src), 64 * n + 64) ||
        overlaps(LSQ(dst), 256 * n, LSQ(wts), 64 * n + 64))
        return 0;
    for (; n; n--, src += 64, wts += 64, dst += 256) {
        __m128 o[16];
        unpack16(ls, src, wts, o);
        for (int p = 0; p < 16; p++)
            _mm_storeu_si128((__m128i*)(ls + LSQ(dst + 16 * p)), bswap32x4(_mm_castps_si128(o[p])));
    }
    return 1;
}

/* The 36 taps of the gather in the order the SPU code adds them (the table pairs at LS 0x6630/0x6870 as the
 * software pipeline consumes them): (ky, kx), each times 512 in the tables. A tap's offset in pixels is
 * (512 k * radius) >> 16 (mpys: signed 16x16 product, arithmetic shift), rows times the row stride. */
static const int8_t dof_taps[36][2] = {
    { 0, -2}, {-1, -2}, {-2, -2}, { 1, -3}, { 0, -3}, {-1, -3}, { 0, -1}, {-1, -1}, {-2, -1}, {-3, -1}, { 2, -2}, { 1, -2},
    {-1,  0}, {-2,  0}, {-3,  0}, { 3, -1}, { 2, -1}, { 1, -1}, {-1,  1}, {-2,  1}, {-3,  1}, { 3,  0}, { 2,  0}, { 1,  0},
    {-1,  2}, {-2,  2}, { 3,  1}, { 2,  1}, { 1,  1}, { 0,  1}, { 1,  3}, { 0,  3}, {-1,  3}, { 2,  2}, { 1,  2}, { 0,  2},
};

static inline int32_t mpys(int32_t a, int32_t b)
{
    return ((int32_t)(int16_t)a * (int32_t)(int16_t)b) >> 16;
}

static void tap_offsets(int32_t* off, int32_t r, int32_t stride)
{
    for (int t = 0; t < 36; t++) {
        const int32_t dy = mpys(dof_taps[t][0] * 512, r), dx = mpys(dof_taps[t][1] * 512, r);
        off[t] = (int32_t)(uint32_t)((uint32_t)((int32_t)(int16_t)dy * stride) + (uint32_t)((int32_t)(int16_t)dx * 16));
    }
}

/* Tap offsets for every radius at a row stride, built once per stride (a pass's band keeps its stride; a level
 * uses a few): 256 x 36 offsets. Published with a release store after building; never freed. */
typedef struct { int32_t stride; int32_t off[256][36]; } dof_offtab;
static dof_offtab* volatile s_offtab[8];
static volatile long s_offtab_lock;

static const int32_t* offsets_for(int32_t stride)
{
    for (int i = 0; i < 8; i++) {
        dof_offtab* t = __atomic_load_n(&s_offtab[i], __ATOMIC_ACQUIRE);
        if (!t) break;
        if (t->stride == stride) return &t->off[0][0];
    }
    while (__sync_lock_test_and_set(&s_offtab_lock, 1)) { }
    const int32_t* r = NULL;
    for (int i = 0; i < 8 && !r; i++) {
        dof_offtab* t = s_offtab[i];
        if (t && t->stride == stride) r = &t->off[0][0];
        else if (!t) {
            t = (dof_offtab*)malloc(sizeof *t);
            if (!t) break;
            t->stride = stride;
            for (int rr = 0; rr < 256; rr++) tap_offsets(t->off[rr], rr, stride);
            __atomic_store_n(&s_offtab[i], t, __ATOMIC_RELEASE);
            r = &t->off[0][0];
        }
    }
    __sync_lock_release(&s_offtab_lock);
    return r;   /* NULL: 8 strides seen already (the caller computes its own) */
}

static inline int ovl(int64_t a, int64_t an, int64_t b, int64_t bn)
{
    return an > 0 && bn > 0 && a < b + bn && b < a + an;
}

static inline int32_t offtab_min(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t offtab_max(int32_t a, int32_t b) { return a > b ? a : b; }

/* One pixel of phase 1 with the helpers (inputs that may reach exponent 255, or a window that wraps the LS). */
static u128 gather_ref(const uint8_t* ls, uint32_t lane, const int32_t* off)
{
    u128 acc = ldq(ls, lane);
    for (int t = 0; t < 36; t++) acc = spu_fa(acc, ldq(ls, lane + (uint32_t)off[t]));
    return acc;
}

/* Output byte 4p+j (raw, big-endian) = the low byte of channel j (host lane j, byte 4j). */
static const uint8_t pack_mask[4][16] __attribute__((aligned(16))) = {
    {0, 4, 8, 12, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128},
    {128, 128, 128, 128, 0, 4, 8, 12, 128, 128, 128, 128, 128, 128, 128, 128},
    {128, 128, 128, 128, 128, 128, 128, 128, 0, 4, 8, 12, 128, 128, 128, 128},
    {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 0, 4, 8, 12},
};

/* Per-thread host-order copy of the rows a gather call can reach (12 rows around the call's row, 6 pixels either
 * side), so the 37 reads per pixel need no byte swap. */
#define WIN_MAX (64 * 1024)
static __thread uint8_t* t_win;

/* @0x5750: r3 = RGBA8 destination, r4 = float4 pixels (the unpacked band), r5 = per-pixel control words (top byte =
 * blur radius), r6 = float4 scratch, r7 = row stride in bytes (each lane, used as a signed halfword), r8 = pixels
 * / 4 (a multiple of 4).
 * Phase 1 (loops 0x57A0/0x5908, 4 pixels a step): acc = centre + the 36 taps at radius-scaled offsets, in the SPU's
 * order; when the 4 radii are all 0 the step stores fma(c, 0x00000024 (a denormal: 0), c) for each pixel instead.
 * Phase 2 (loop 0x5BD8, 16 pixels a step): n = acc * 1/acc.x (frest + fi), a = min(16 r/255, 1),
 * out = clamp(cfltu(n * a * 0.99609375, 2^8), 255) per channel, the low bytes packed as 4 bytes per pixel.
 * Fast path: when no word in the reachable rows has a biased exponent above 247 (|x| < 2^121), no sum of 37 of them
 * can reach exponent 255, and the adds are plain SSE adds from the host-order copy; otherwise each pixel runs with
 * the helpers. */
static int dof_gather_5750(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t out = ctx->gpr[3]._u32[0], work = ctx->gpr[4]._u32[0], ctl = ctx->gpr[5]._u32[0];
    const uint32_t tmp = ctx->gpr[6]._u32[0], n = ctx->gpr[8]._u32[0];
    const int32_t st = (int16_t)ctx->gpr[7]._u32[0];
    /* (a stride that is not a multiple of 16 would need the SPU's masking of each tap address: lifted code then) */
    int ok = n != 0 && !(n & 3) && n <= 1024 && !(st & 15);
    if (!ok) return 0;
    /* Reach of the taps: a tap's offset is monotonic in the radius, so the extremes over the call's pixels are those
     * at its largest radius (or 0). The window is everything from the lowest to the highest LS address read. */
    const int32_t* offtab = offsets_for(st);
    uint32_t rmax = 0;
    for (uint32_t i = 0; i < n; i++) {
        const u128 c = ldq(ls, ctl + 16 * i);
        for (int k = 0; k < 4; k++) rmax = (c._u32[k] >> 24) > rmax ? (c._u32[k] >> 24) : rmax;
    }
    int32_t off_local[36], last_r = -1, omin = 0, omax = 0;
    {
        const int32_t* o = offtab ? offtab + 36 * rmax : off_local;
        if (!offtab) tap_offsets(off_local, (int32_t)rmax, st);
        for (int t = 0; t < 36; t++) {
            omin = o[t] < omin ? o[t] : omin;
            omax = o[t] > omax ? o[t] : omax;
        }
    }
    const int64_t wlo = (int64_t)(work & ~15u) + omin;
    const int64_t whi = (int64_t)(work & ~15u) + 64 * (int64_t)n + omax;
    for (int k = 1; k < 4 && ok; k++) ok = (int16_t)ctx->gpr[7]._u32[k] == st;
    ok = ok && !overlaps(LSQ(tmp), 64 * n, LSQ(out), 16 * n) && !overlaps(LSQ(tmp), 64 * n, LSQ(ctl), 16 * n + 64) &&
         !overlaps(LSQ(out), 16 * n, LSQ(ctl), 16 * n + 64);
    /* the scratch and the output must not overlap anything the gather reads */
    ok = ok && wlo >= 0 && whi <= SPU_LS_SIZE && whi - wlo <= WIN_MAX &&
         !overlaps(LSQ(tmp), 64 * n, (uint32_t)wlo, (uint32_t)(whi - wlo)) &&
         !overlaps(LSQ(out), 16 * n, (uint32_t)wlo, (uint32_t)(whi - wlo));
    if (!ok) return 0;
    if (!t_win && !(t_win = (uint8_t*)_mm_malloc(WIN_MAX, 64))) return 0;
    /* host-order copy of the window, and its largest biased exponent */
    uint8_t* win = t_win;
    __m128i emax = _mm_setzero_si128();
    const __m128i em = _mm_set1_epi32(0x7F800000);
    for (int64_t a = wlo; a < whi; a += 16) {
        const __m128i v = bswap32x4(_mm_loadu_si128((const __m128i*)(ls + a)));
        emax = _mm_max_epi32(emax, _mm_and_si128(v, em));
        _mm_store_si128((__m128i*)(win + (a - wlo)), v);
    }
    emax = _mm_max_epi32(emax, _mm_shuffle_epi32(emax, _MM_SHUFFLE(1, 0, 3, 2)));
    emax = _mm_max_epi32(emax, _mm_shuffle_epi32(emax, _MM_SHUFFLE(2, 3, 0, 1)));
    const int clean = _mm_cvtsi128_si32(emax) <= 0x7B800000;
    const uint8_t* wb = win - wlo;   /* wb + lsa = host copy of LS lsa, inside the window */

    for (uint32_t i = 0; i < n; i++) {
        const u128 c = ldq(ls, ctl + 16 * i);
        const uint32_t base = work + 64 * i;
        const uint32_t rad[4] = { c._u32[0] >> 24, c._u32[1] >> 24, c._u32[2] >> 24, c._u32[3] >> 24 };
        if ((rad[0] | rad[1] | rad[2] | rad[3]) == 0) {
            const u128 denorm = splat(0x24u);
            for (int k = 0; k < 4; k++) {
                const u128 v = ldq(ls, base + 16 * k);
                stq(ls, tmp + 64 * i + 16 * k, spu_fma(v, denorm, v));
            }
            continue;
        }
        for (int k = 0; k < 4; k++) {
            const int32_t r = (int32_t)rad[k];
            const int32_t* off = offtab ? offtab + 36 * r : off_local;
            if (!offtab && r != last_r) {
                tap_offsets(off_local, r, st);
                last_r = r;
            }
            const uint32_t lane = base + 16u * (uint32_t)k;
            if (!clean) {
                stq(ls, tmp + 64 * i + 16 * k, gather_ref(ls, lane, off));
                continue;
            }
            const uint8_t* p = wb + (lane & ~15u);
            __m128 acc = _mm_load_ps((const float*)p);
            for (int t = 0; t < 36; t++) acc = _mm_add_ps(acc, _mm_load_ps((const float*)(p + off[t])));
            _mm_storeu_si128((__m128i*)(ls + LSQ(tmp + 64 * i + 16 * k)), bswap32x4(_mm_castps_si128(acc)));
        }
    }

    const __m128 k255 = f4(K_255), k16 = f4(0x41800000u), one = f4(0x3F800000u), kq = f4(0x3F7F0000u),
                 s8 = f4(0x3B800000u);
    for (uint32_t g = 0; g < n / 4; g++) {
        for (int q = 0; q < 4; q++) {
            /* blend factor of the 4 pixels: min(r/256 * 256/255 * 16, 1) (fm, fm, fcgt + selb) */
            const __m128i craw = _mm_loadu_si128((const __m128i*)(ls + LSQ(ctl + 64 * g + 16 * q)));
            const __m128i rb = _mm_shuffle_epi8(craw, _mm_set_epi8(-1, -1, -1, 12, -1, -1, -1, 8, -1, -1, -1, 4, -1, -1, -1, 0));
            __m128 f = _mm_mul_ps(_mm_mul_ps(_mm_mul_ps(_mm_cvtepi32_ps(rb), s8), k255), k16);
            f = _mm_blendv_ps(f, one, _mm_cmpgt_ps(f, one));
            /* Radius 0 (in focus): f = +0, and with the SPU's extended range on (g_kz3_spu_xr, the default) every
             * fm by +0 is +-0 (an exponent-255 lane is fixed to +0 by spu__fm_lane), fm(+-0, 0.996) is +-0 and
             * cfltu(+-0) is 0: the pixel is 4 zero bytes whatever its accumulator holds. */
            const int zmask = g_kz3_spu_xr ? _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_castps_si128(f), _mm_setzero_si128()))) : 0;
            if (zmask == 15) {
                _mm_storeu_si128((__m128i*)(ls + LSQ(out + 64 * g + 16 * q)), _mm_setzero_si128());
                continue;
            }
            /* the 4 pixels' accumulators; one frest + fi for their 4 weights (lane 0 of each) */
            __m128 sv[4];
            for (int p = 0; p < 4; p++)
                sv[p] = _mm_castsi128_ps(bswap32x4(_mm_loadu_si128((const __m128i*)(ls + LSQ(tmp + 256 * g + 64 * q + 16 * p)))));
            const __m128 w01 = _mm_unpacklo_ps(sv[0], sv[1]), w23 = _mm_unpacklo_ps(sv[2], sv[3]);
            const __m128 w = _mm_movelh_ps(w01, w23);
            const __m128 rcp = to_f(spu_fi(to_u(w), spu_frest(to_u(w))));
            __m128i px = _mm_setzero_si128();
            for (int p = 0; p < 4; p++) {
                if (zmask >> p & 1) continue;
                const __m128 rs = lane_splat(rcp, p), fs = lane_splat(f, p);
                const __m128 v = fm_x(fm_x(fm_x(sv[p], rs), fs), kq);
                /* cfltu(v, 165) = v * 2^8 truncated, clamped to [0, 2^32 - 1]; plain when 0 <= v < 2^23 */
                __m128i u;
                const __m128i vb = _mm_castps_si128(v);
                if (_mm_movemask_epi8(_mm_cmpgt_epi32(vb, _mm_set1_epi32(0x4AFFFFFF))) == 0 && _mm_movemask_ps(v) == 0)
                    u = _mm_cvttps_epi32(_mm_mul_ps(v, f4(0x43800000u)));
                else {
                    const u128 uu = spu_cfltu(to_u(v), 165);
                    memcpy(&u, &uu, 16);
                }
                /* (signed) > 255 -> 255; the low bytes of the 4 channels are the pixel's 4 bytes */
                u = _mm_blendv_epi8(u, _mm_set1_epi32(255), _mm_cmpgt_epi32(u, _mm_set1_epi32(255)));
                px = _mm_or_si128(px, _mm_shuffle_epi8(u, _mm_load_si128((const __m128i*)pack_mask[p])));
            }
            _mm_storeu_si128((__m128i*)(ls + LSQ(out + 64 * g + 16 * q)), px);
        }
    }
    return 1;
}

/* ---- The whole pass @0x4A20 (default off: KZ2_NATIVE_JOB_DOFPASS=1, or KZ2_NATIVE_JOB_POST=1) -------
 * The pass (disassembly @0x4A20-0x4C8C; the RE notes "An earlier change"): parameters q0 = the quadword at r3, q1 = at
 * r3 + 0x10; A = q0 byte 4 (pixels per buffer row: 96), B = q0 byte 7 (bytes per source pixel: 4), W = q1 byte 4 (output
 * pixels per row), R = q1 byte 5 (rows). r5 = source pixels, r6 = control words (top byte = blur radius; the unpack's
 * weights are the same words), r9 = RGBA8 output. Bands of up to 15 rows: for band b (output rows 15 b ..), the unpack
 * @0x5F80 runs over band rows k = 0 .. r84 - 1 (r84 = rows + 14) with n = 6 (96 pixels) from source row 15 b + k of
 * the apron-extended image (px0 = r5 - 32 - 7 AB, weights w0 = r6 - 32 - 7 AB, 384 bytes a row) into the float buffer
 * at LS 0x6CC0 (1536 bytes a row), then the gather @0x5750 once per output row i (out = r9 + 4 W g, ctl = r6 + 4 A g
 * for global row g = 15 b + i, work = 0x6CC0 + 128 + 16 A (7 + i), scratch 0x11AC0, stride 16 A, n = W >> 2).
 * The SPU unpacks each apron row twice (once per band it belongs to) and the gather sums every pixel's taps even where
 * phase 2 throws them away. The native pass keeps every unpacked row in a host-order buffer by its global row (each
 * computed at most once, and only when something reads it), skips phase 1 for a group of four pixels whose radii are
 * all 0 when phase 2 does not read it (the SPU extended range on: such pixels are four zero bytes whatever their sums),
 * and writes the local store as the SPU code leaves it: the output rows, the float buffer as the last two bands left
 * it (rows 0 .. r84 - 1 of the last band, rows r84 .. 28 of the band before), the scratch of the last gather call, and
 * the stack frame (r80-r98 saved, r0, the back chain at r1 - 336). It declines up front (the lifted pass runs, with
 * the native kernels) unless A = 96, B = 4, W >> 2 is a positive multiple of 4, every tap of every row stays inside
 * its band's buffer rows, and what it writes (output, buffer, scratch, frame) overlaps nothing it reads (sources,
 * weights, control words, parameters, the job's constants) nor each other: then each unpacked row is the same in
 * every band, which is what makes computing it once exact. */
#define DOF_BUF 0x6CC0u
#define DOF_SCR 0x11AC0u
#define DOF_ROWB 1536u

static __thread uint8_t* t_rows;        /* host-order unpacked rows by global row, DOF_ROWB bytes each */
static __thread uint8_t* t_rowdone;     /* per row: 0 not yet, 1 unpacked */
static __thread uint32_t t_rowcap;

static inline void ensure_row(const uint8_t* ls, uint32_t px0, uint32_t w0, uint32_t s)
{
    if (t_rowdone[s]) return;
    __m128* o = (__m128*)(t_rows + (size_t)s * DOF_ROWB);
    for (uint32_t g = 0; g < 6; g++) unpack16(ls, px0 + 384 * s + 64 * g, w0 + 384 * s + 64 * g, o + 16 * g);
    t_rowdone[s] = 1;
}

/* The radius of control word j (top byte) of the 4n words at ctl. */
static inline uint32_t rad_of(const uint8_t* ls, uint32_t ctl, uint32_t j)
{
    return ls[LSQ(ctl + 16 * (j >> 2)) + 4 * (j & 3)];
}

static int dof_pass_4A20(spu_context* ctx)
{
    uint8_t* ls = ctx->ls;
    const uint32_t r1 = ctx->gpr[1]._u32[0], prm = ctx->gpr[3]._u32[0];
    const uint32_t px = ctx->gpr[5]._u32[0], ctl = ctx->gpr[6]._u32[0], out = ctx->gpr[9]._u32[0];
    const uint8_t* q0 = ls + LSQ(prm);
    const uint8_t* q1 = ls + LSQ(prm + 16);
    const uint32_t A = q0[4], B = q0[7], W = q1[4], R = q1[5], n = W >> 2;
    if (A != 96 || B != 4 || (r1 & 15) || (int64_t)r1 - 336 < 0 || r1 + 32 > SPU_LS_SIZE) return 0;
    const int64_t fr0 = (int64_t)r1 - 336, frn = 368;
    const int32_t* ot = NULL;   /* tap offsets at the buffer's row stride (1536) */
    if (R) {
        if (n == 0 || (n & 3)) return 0;
        const int64_t px0 = (int64_t)px - 32 - 7 * 384, w0 = (int64_t)ctl - 32 - 7 * 384, nr = R + 14;
        /* reads (the +64: the kernels' own over-read margins) and writes */
        const int64_t rd[7][2] = { { px0, 384 * nr + 64 }, { w0, 384 * nr + 64 }, { ctl, 384 * (int64_t)(R - 1) + 16 * n + 64 },
                                   { LSQ(prm), 32 }, { DOF_CONST_LO, DOF_CONST_HI - DOF_CONST_LO },
                                   { DOF_BUF, 29 * DOF_ROWB }, { DOF_SCR, 64 * (int64_t)n } };
        const int64_t wr[4][2] = { { out, 4 * (int64_t)W * (R - 1) + 16 * n }, { DOF_BUF, 29 * DOF_ROWB },
                                   { DOF_SCR, 64 * (int64_t)n }, { fr0, frn } };
        for (int i = 0; i < 7; i++)
            if (rd[i][0] < 0 || rd[i][0] + rd[i][1] > SPU_LS_SIZE) return 0;
        for (int i = 0; i < 4; i++) {
            if (wr[i][0] < 0 || wr[i][0] + wr[i][1] > SPU_LS_SIZE) return 0;
            for (int j = 0; j < 5; j++)
                if (ovl(wr[i][0], wr[i][1], rd[j][0], rd[j][1])) return 0;
            for (int j = i + 1; j < 4; j++)
                if (ovl(wr[i][0], wr[i][1], wr[j][0], wr[j][1])) return 0;
        }
        /* the taps' reach at the largest radius of the pass, inside every band's rows */
        uint32_t rmax = 0;
        for (uint32_t g = 0; g < R; g++)
            for (uint32_t j = 0; j < 4 * n; j++) {
                const uint32_t r = rad_of(ls, ctl + 384 * g, j);
                rmax = r > rmax ? r : rmax;
            }
        if (!(ot = offsets_for(1536))) return 0;
        int32_t omin = 0, omax = 0;
        for (int t = 0; t < 36; t++) {
            omin = offtab_min(omin, ot[36 * rmax + t]);
            omax = offtab_max(omax, ot[36 * rmax + t]);
        }
        if (128 + 7 * 1536 + omin < 0 || 128 + 64 * (int64_t)n + omax > 8 * 1536) return 0;
        if (t_rowcap < nr) {
            uint8_t* nrw = (uint8_t*)_mm_malloc((size_t)nr * DOF_ROWB, 64);
            uint8_t* nd = (uint8_t*)malloc(nr);
            if (!nrw || !nd) { _mm_free(nrw); free(nd); return 0; }
            _mm_free(t_rows); free(t_rowdone);
            t_rows = nrw; t_rowdone = nd; t_rowcap = (uint32_t)nr;
        }
        memset(t_rowdone, 0, nr);
    }

    /* the frame: r80-r98 below r1, r0 at r1 + 16, the back chain at r1 - 336 */
    for (int k = 80; k <= 98; k++) stq(ls, r1 - 16 * (uint32_t)(k - 79), ctx->gpr[k]);
    stq(ls, r1 + 16, ctx->gpr[0]);
    stq(ls, r1 - 336, ctx->gpr[1]);
    if (!R) return 1;

    const uint32_t px0 = px - 32 - 7 * 384, w0 = ctl - 32 - 7 * 384;
    const int xr = g_kz3_spu_xr;
    const u128 denorm = splat(0x24u);
    __m128 acc[256];
    uint32_t rem = R, band = 0, gl = 0;
    while (rem) {
        const uint32_t rows = rem > 14 ? 15 : rem, r84 = rows + 14;
        const uint8_t* hb = t_rows + (size_t)15 * band * DOF_ROWB - DOF_BUF;   /* hb + LS address in the band's buffer */
        for (uint32_t i = 0; i < rows; i++, gl++) {
            const int last = rem == rows && i == rows - 1;
            const uint32_t c0 = ctl + 384 * gl, o0 = out + 4 * W * gl, work = DOF_BUF + 128 + 1536 * (7 + i);
            /* phase 1 */
            uint32_t rcall = 0;
            for (uint32_t j = 0; j < 4 * n; j++) {
                const uint32_t r = rad_of(ls, c0, j);
                rcall = r > rcall ? r : rcall;
            }
            if (rcall || last || !xr) {   /* the rows this call can read */
                int32_t lo = 0, hi = 0;
                for (int t = 0; t < 36; t++) {
                    lo = offtab_min(lo, ot[36 * rcall + t]);
                    hi = offtab_max(hi, ot[36 * rcall + t]);
                }
                const uint32_t k0 = (uint32_t)(((int64_t)work + lo - DOF_BUF) / DOF_ROWB);
                const uint32_t k1 = (uint32_t)(((int64_t)work + 64 * n + hi - 1 - DOF_BUF) / DOF_ROWB);
                for (uint32_t k = k0; k <= k1 && k < r84; k++) ensure_row(ls, px0, w0, 15 * band + k);
            }
            for (uint32_t ig = 0; ig < n; ig++) {
                uint32_t rad[4];
                for (int k = 0; k < 4; k++) rad[k] = rad_of(ls, c0, 4 * ig + (uint32_t)k);
                const int zero = (rad[0] | rad[1] | rad[2] | rad[3]) == 0;
                if (zero && xr && !last) continue;   /* phase 2 stores 0 for these four pixels; nothing reads their sums */
                const uint32_t base = work + 64 * ig;
                for (int k = 0; k < 4; k++) {
                    const uint8_t* p = hb + base + 16 * (uint32_t)k;
                    __m128 a = _mm_load_ps((const float*)p);
                    if (zero) {
                        a = to_f(spu_fma(to_u(a), denorm, to_u(a)));
                    } else {
                        const int32_t* off = ot + 36 * rad[k];
                        for (int t = 0; t < 36; t++) a = _mm_add_ps(a, _mm_load_ps((const float*)(p + off[t])));
                    }
                    acc[4 * ig + (uint32_t)k] = a;
                    if (last) _mm_storeu_si128((__m128i*)(ls + DOF_SCR + 64 * ig + 16 * (uint32_t)k), bswap32x4(_mm_castps_si128(a)));
                }
            }
            /* phase 2 (as dof_gather_5750's, from the host sums) */
            const __m128 k255 = f4(K_255), k16 = f4(0x41800000u), one = f4(0x3F800000u), kq = f4(0x3F7F0000u),
                         s8 = f4(0x3B800000u);
            for (uint32_t g = 0; g < n / 4; g++) {
                for (int q = 0; q < 4; q++) {
                    const __m128i craw = _mm_loadu_si128((const __m128i*)(ls + LSQ(c0 + 64 * g + 16 * (uint32_t)q)));
                    const __m128i rb = _mm_shuffle_epi8(craw, _mm_set_epi8(-1, -1, -1, 12, -1, -1, -1, 8, -1, -1, -1, 4, -1, -1, -1, 0));
                    __m128 f = _mm_mul_ps(_mm_mul_ps(_mm_mul_ps(_mm_cvtepi32_ps(rb), s8), k255), k16);
                    f = _mm_blendv_ps(f, one, _mm_cmpgt_ps(f, one));
                    const int zmask = xr ? _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_castps_si128(f), _mm_setzero_si128()))) : 0;
                    if (zmask == 15) {
                        _mm_storeu_si128((__m128i*)(ls + LSQ(o0 + 64 * g + 16 * (uint32_t)q)), _mm_setzero_si128());
                        continue;
                    }
                    const __m128* sv = &acc[16 * g + 4 * (uint32_t)q];
                    const __m128 w01 = _mm_unpacklo_ps(sv[0], sv[1]), w23 = _mm_unpacklo_ps(sv[2], sv[3]);
                    const __m128 w = _mm_movelh_ps(w01, w23);
                    const __m128 rcp = to_f(spu_fi(to_u(w), spu_frest(to_u(w))));
                    __m128i pxv = _mm_setzero_si128();
                    for (int p = 0; p < 4; p++) {
                        if (zmask >> p & 1) continue;
                        const __m128 rs = lane_splat(rcp, p), fs = lane_splat(f, p);
                        const __m128 v = fm_x(fm_x(fm_x(sv[p], rs), fs), kq);
                        __m128i u;
                        const __m128i vb = _mm_castps_si128(v);
                        if (_mm_movemask_epi8(_mm_cmpgt_epi32(vb, _mm_set1_epi32(0x4AFFFFFF))) == 0 && _mm_movemask_ps(v) == 0)
                            u = _mm_cvttps_epi32(_mm_mul_ps(v, f4(0x43800000u)));
                        else {
                            const u128 uu = spu_cfltu(to_u(v), 165);
                            memcpy(&u, &uu, 16);
                        }
                        u = _mm_blendv_epi8(u, _mm_set1_epi32(255), _mm_cmpgt_epi32(u, _mm_set1_epi32(255)));
                        pxv = _mm_or_si128(pxv, _mm_shuffle_epi8(u, _mm_load_si128((const __m128i*)pack_mask[p])));
                    }
                    _mm_storeu_si128((__m128i*)(ls + LSQ(o0 + 64 * g + 16 * (uint32_t)q)), pxv);
                }
            }
        }
        rem -= rows;
        if (!rem) {
            /* the float buffer as the SPU leaves it: this band's rows, then the band before's rows past them */
            for (uint32_t k = 0; k < 29; k++) {
                uint32_t s;
                if (k < r84) s = 15 * band + k;
                else if (band) s = 15 * (band - 1) + k;
                else break;
                ensure_row(ls, px0, w0, s);
                const __m128i* h = (const __m128i*)(t_rows + (size_t)s * DOF_ROWB);
                for (uint32_t x = 0; x < 96; x++)
                    _mm_storeu_si128((__m128i*)(ls + DOF_BUF + DOF_ROWB * k + 16 * x), bswap32x4(_mm_load_si128(h + x)));
            }
        }
        band++;
    }
    return 1;
}

/* @0x4CB0, the blend of the blurred layer over the sharp one, is the Gfx.Post family kernel "blend" that all 16
 * Gfx.Post images share (native_post.c, kz2_post_blend); an earlier change wrote it here first. */
int kz2_post_blend(spu_context* ctx);

#define DOF_ENV "KZ2_NATIVE_JOB_DOF"
const kz2_native_entry kz2_native_dof[] = {
    { 0, DOF_ENV, "Gfx.Post.DepthOfField", "job_00d6d100", 0x5750u, dof_gather_5750, "dof_gather", DOF_CONST_LO,
      DOF_CONST_HI, DOF_CONST_HASH, 0 },
    { 1, DOF_ENV, "Gfx.Post.DepthOfField", "job_00d6d100", 0x5F80u, dof_unpack_5F80, "dof_unpack", DOF_CONST_LO,
      DOF_CONST_HI, DOF_CONST_HASH, 0 },
    { 2, DOF_ENV "|KZ2_NATIVE_JOB_POSTMIX", "Gfx.Post.DepthOfField", "job_00d6d100", 0x4CB0u, kz2_post_blend, "blend",
      0x6550u, 0x6620u, 0xb65df4abc0d86d6full, 0 },
    { 74, "KZ2_NATIVE_JOB_DOFPASS", "Gfx.Post.DepthOfField", "job_00d6d100", 0x4A20u, dof_pass_4A20, "dofpass",
      DOF_CONST_LO, DOF_CONST_HI, DOF_CONST_HASH, 0 },
};
const unsigned kz2_native_dof_n = sizeof kz2_native_dof / sizeof kz2_native_dof[0];
