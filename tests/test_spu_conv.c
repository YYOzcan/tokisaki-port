/* Compare the x86-64-v3 shufb, fi and cflts/cfltu/csflt/cuflt against the
 * scalar references, on random words, special exponents and every scale. */
#include "spu_helpers.h"
#include <stdio.h>
static uint64_t s = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)s; }
static uint32_t rw(void)
{
    switch (rnd() % 8) {
    case 0: return 0;
    case 1: return rnd() & 0x807FFFFFu;                                  /* exponent 0 */
    case 2: return rnd() | 0x7F800000u;                                  /* exponent 255 */
    case 3: return (rnd() & 0x807FFFFFu) | ((uint32_t)(110 + rnd() % 40) << 23);
    case 4: return rnd() % 4096;                                          /* small integers */
    case 5: return 0u - rnd() % 4096;
    default: return rnd();
    }
}
static u128 rv(void) { u128 v; for (int i = 0; i < 4; i++) v._u32[i] = rw(); return v; }
static int eq(u128 a, u128 b) { return !memcmp(&a, &b, 16); }
int main(void)
{
    long bad = 0;
    for (int pass = 0; pass < 2; pass++) {          /* pass 1: the lifted code's MXCSR mode */
    if (pass) {
        g_spu_fp_saved = _mm_getcsr();
        _mm_setcsr((g_spu_fp_saved & ~0x6000u) | 0x6000u | 0x0040u | 0x8000u);
        g_spu_fp_rz = 1;
    }
    for (long n = 0; n < 3000000; n++) {
        u128 a = rv(), b = rv(), c = rv();
        if (n & 1) for (int i = 0; i < 4; i++) c._u32[i] &= 0x1F1F1F1Fu;   /* plain selectors */
        const int i8 = (int)(rnd() & 0xFF);
#define T(name, x, y) if (!eq(x, y) && bad++ < 20) printf("%s mismatch i8=%d %08X %08X %08X\n", name, i8, a._u32[0], b._u32[0], c._u32[0]);
        T("shufb", spu_shufb(a, b, c), spu_shufb_ref(a, b, c));
        T("fi", spu_fi(a, b), spu_fi_ref(a, b));
        T("cflts", spu_cflts(a, i8), spu_cflts_ref(a, i8));
        T("cfltu", spu_cfltu(a, i8), spu_cfltu_ref(a, i8));
        T("csflt", spu_csflt(a, i8), spu_csflt_ref(a, i8));
        T("cuflt", spu_cuflt(a, i8), spu_cuflt_ref(a, i8));
    }
    }
    printf("%ld mismatches\n", bad);
    return bad != 0;
}
