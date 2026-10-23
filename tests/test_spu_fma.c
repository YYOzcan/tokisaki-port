/* Compare the x86-64-v3 float fast paths (fa/fs/fm/fma/fms/fnms) against the
 * exact reference, on random singles and on the kind of operands an image
 * filter feeds them: small multiples of 1/256 whose results are exact. */
#include "spu_helpers.h"
#include <stdio.h>
static uint64_t s = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)s; }
static uint32_t rf(void)
{
    switch (rnd() % 9) {
    case 7: return (rnd() & 0x807FFFFFu) | ((uint32_t)(61 + rnd() % 6) << 23);     /* products near 2^-126 */
    case 8: return (rnd() & 0x807FFFFFu) | ((uint32_t)(189 + rnd() % 6) << 23);    /* products near 2^128 */
    case 0: return 0;
    case 1: return 0x80000000u;
    case 2: { float f = (float)((int)(rnd() % 2048) - 1024) / 256.0f; uint32_t u; memcpy(&u, &f, 4); return u; }
    case 3: { float f = (float)(rnd() % 256) / 255.0f; uint32_t u; memcpy(&u, &f, 4); return u; }
    case 4: return (rnd() & 0x807FFFFFu) | ((uint32_t)(100 + rnd() % 56) << 23);   /* near 1.0 */
    case 5: return (rnd() & 1) ? (rnd() & 0x807FFFFFu)                              /* denormal */
                               : (rnd() | 0x7F800000u);                             /* extended */
    default: return rnd();
    }
}
static u128 rv(void) { u128 v; for (int i = 0; i < 4; i++) v._u32[i] = rf(); return v; }
static int eq(u128 a, u128 b) { return !memcmp(&a, &b, 16); }
int main(void)
{
    long bad = 0;
    for (long n = 0; n < 8000000; n++) {
        u128 a = rv(), b = rv(), c = rv();
#define T(name, x, y) if (!eq(x, y) && bad++ < 20) printf("%s mismatch %08X %08X %08X\n", name, a._u32[0], b._u32[0], c._u32[0]);
        T("fa", spu_fa(a, b), spu_fa_ref(a, b));
        T("fs", spu_fs(a, b), spu_fs_ref(a, b));
        T("fm", spu_fm(a, b), spu_fm_ref(a, b));
        T("fma", spu_fma(a, b, c), spu_fma_ref(a, b, c));
        T("fms", spu_fms(a, b, c), spu_fms_ref(a, b, c));
        T("fnms", spu_fnms(a, b, c), spu_fnms_ref(a, b, c));
    }
    printf("%ld mismatches\n", bad);
    return bad != 0;
}
