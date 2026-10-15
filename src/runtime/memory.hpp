#pragma once

#include "common/types.hpp"
#include "common/endian.hpp"
#include <span>
#include <string_view>

namespace tokisaki::memory {

constexpr u64 GUEST_ADDR_SPACE_SIZE = 0x100000000ULL; // 4 GB address space
constexpr u32 MAIN_RAM_SIZE         = 256 * 1024 * 1024; // 256 MB XDR
constexpr u32 RSX_VRAM_SIZE         = 256 * 1024 * 1024; // 256 MB GDDR3
constexpr GuestAddr RSX_VRAM_BASE   = 0xC0000000;

bool initialize();
void shutdown();

// Translate guest address directly to host pointer
u8* translate(GuestAddr addr);

// Read memory with Big-Endian conversion
template <typename T>
inline T read(GuestAddr addr) {
    auto* ptr = reinterpret_cast<const be_t<T>*>(translate(addr));
    return static_cast<T>(*ptr);
}

// Write memory with Big-Endian conversion
template <typename T>
inline void write(GuestAddr addr, T value) {
    auto* ptr = reinterpret_cast<be_t<T>*>(translate(addr));
    *ptr = value;
}

// Raw copy
void copy_to_guest(GuestAddr dst, const void* src, size_t size);
void copy_from_guest(void* dst, GuestAddr src, size_t size);

// Returns host base pointer for guest space
u8* get_base_ptr();

} // namespace tokisaki::memory
