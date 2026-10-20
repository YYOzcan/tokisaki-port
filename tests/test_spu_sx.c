/* Compare the SSE byte-shift and multiply helpers against the reference loops. */
#include "spu_helpers.h"
#include <stdio.h>
static uint64_t s = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)s; }
static u128 rv(void) { u128 v; for (int i = 0; i < 4; i++) { uint32_t r = rnd(); if ((rnd() & 7) == 0) r = (rnd() & 1) ? 0x80008000u : 0xFFFFFFFFu; v._u32[i] = r; } return v; }
static int eq(u128 a, u128 b) { return !memcmp(&a, &b, 16); }
int main(void)
{
    long bad = 0;
    for (long n = 0; n < 2000000; n++) {
        u128 a = rv(), b = rv(), c = rv();
        int k = (int)(rnd() % 128) - 64;
        u128 bs = b; bs._u32[0] = (uint32_t)k ^ (rnd() & 0xFFFFFF00u);
#define T(name, x, y) if (!eq(x, y) && bad++ < 20) printf("%s mismatch k=%d\n", name, k);
        T("shlqbyi", spu_shlqbyi(a, k), spu_shlqbyi_ref(a, k));
        T("rotqbyi", spu_rotqbyi(a, k), spu_rotqbyi_ref(a, k));
        T("shlqby", spu_shlqby(a, bs), spu_shlqby_ref(a, bs));
        T("rotqby", spu_rotqby(a, bs), spu_rotqby_ref(a, bs));
        T("shlqbybi", spu_shlqbybi(a, bs), spu_shlqbybi_ref(a, bs));
        T("rotqbybi", spu_rotqbybi(a, bs), spu_rotqbybi_ref(a, bs));
        T("rotqmby", spu_rotqmby(a, bs), spu_rotqmby_ref(a, bs));
        T("rotqmbybi", spu_rotqmbybi(a, bs), spu_rotqmbybi_ref(a, bs));
        T("rotqmbyi", spu_rotqmbyi(a, k), spu_rotqmbyi_ref(a, k));
        T("mpy", spu_mpy(a, b), spu_mpy_ref(a, b));
        T("mpyu", spu_mpyu(a, b), spu_mpyu_ref(a, b));
        T("mpyh", spu_mpyh(a, b), spu_mpyh_ref(a, b));
        T("mpyhh", spu_mpyhh(a, b), spu_mpyhh_ref(a, b));
        T("mpyhhu", spu_mpyhhu(a, b), spu_mpyhhu_ref(a, b));
        T("mpys", spu_mpys(a, b), spu_mpys_ref(a, b));
        T("mpyi", spu_mpyi(a, k * 517), spu_mpyi_ref(a, k * 517));
        T("mpyui", spu_mpyui(a, k * 517), spu_mpyui_ref(a, k * 517));
        T("mpya", spu_mpya(a, b, c), spu_mpya_ref(a, b, c));
        T("mpyhha", spu_mpyhha(a, b, c), spu_mpyhha_ref(a, b, c));
        T("mpyhhau", spu_mpyhhau(a, b, c), spu_mpyhhau_ref(a, b, c));
    }
    printf("test_sx: %ld mismatches\n", bad);
    return bad != 0;
}
