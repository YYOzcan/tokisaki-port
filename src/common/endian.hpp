#pragma once

#include "types.hpp"
#include <bit>
#include <cstring>

namespace tokisaki {

// Host vs Guest Endianness helpers (PS3 is Big-Endian, PC x86_64 is Little-Endian)
constexpr bool is_host_little_endian() noexcept {
    return std::endian::native == std::endian::little;
}

template <typename T>
requires (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8)
constexpr T byte_swap(T value) noexcept {
    if constexpr (sizeof(T) == 1) {
        return value;
    } else if constexpr (sizeof(T) == 2) {
        u16 v;
        std::memcpy(&v, &value, 2);
        v = static_cast<u16>((v << 8) | (v >> 8));
        T result;
        std::memcpy(&result, &v, 2);
        return result;
    } else if constexpr (sizeof(T) == 4) {
        u32 v;
        std::memcpy(&v, &value, 4);
        v = __builtin_bswap32(v);
        T result;
        std::memcpy(&result, &v, 4);
        return result;
    } else if constexpr (sizeof(T) == 8) {
        u64 v;
        std::memcpy(&v, &value, 8);
        v = __builtin_bswap64(v);
        T result;
        std::memcpy(&result, &v, 8);
        return result;
    }
}

// Convert guest (Big-Endian) to host (native)
template <typename T>
constexpr T from_be(T val) noexcept {
    if constexpr (is_host_little_endian()) {
        return byte_swap(val);
    } else {
        return val;
    }
}

// Convert host (native) to guest (Big-Endian)
template <typename T>
constexpr T to_be(T val) noexcept {
    return from_be(val);
}

// Big-Endian wrapper type for direct guest memory mapping
template <typename T>
class be_t {
public:
    be_t() = default;
    constexpr be_t(T val) noexcept : raw_(to_be(val)) {}

    constexpr operator T() const noexcept {
        return from_be(raw_);
    }

    constexpr be_t& operator=(T val) noexcept {
        raw_ = to_be(val);
        return *this;
    }

    constexpr T raw() const noexcept { return raw_; }

private:
    T raw_{};
};

using be_u16 = be_t<u16>;
using be_u32 = be_t<u32>;
using be_u64 = be_t<u64>;
using be_s16 = be_t<s16>;
using be_s32 = be_t<s32>;
using be_s64 = be_t<s64>;
using be_f32 = be_t<f32>;
using be_f64 = be_t<f64>;

} // namespace tokisaki
