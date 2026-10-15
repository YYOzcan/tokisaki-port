#pragma once

#include <cstdint>
#include <cstddef>
#include <concepts>

namespace tokisaki {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using s8  = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

using f32 = float;
using f64 = double;

// Cell SPU / AltiVec 128-bit SIMD vector representation
struct alignas(16) vec128 {
    union {
        u8   u8s[16];
        u16  u16s[8];
        u32  u32s[4];
        u64  u64s[2];
        s8   s8s[16];
        s16  s16s[8];
        s32  s32s[4];
        s64  s64s[2];
        f32  f32s[4];
        f64  f64s[2];
    };

    constexpr bool operator==(const vec128& other) const noexcept {
        return u64s[0] == other.u64s[0] && u64s[1] == other.u64s[1];
    }
};

using GuestAddr = u32;

} // namespace tokisaki
