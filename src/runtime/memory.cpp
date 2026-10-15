#include "memory.hpp"
#include <sys/mman.h>
#include <unistd.h>
#include <iostream>
#include <cstring>

namespace tokisaki::memory {

static u8* s_guest_base = nullptr;

bool initialize() {
    if (s_guest_base != nullptr) {
        return true;
    }

    // Reserve 4GB virtual address space for 1:1 32-bit guest pointer translation
    void* ptr = mmap(nullptr, GUEST_ADDR_SPACE_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

    if (ptr == MAP_FAILED) {
        std::cerr << "[Memory] Failed to map 4GB guest address space!\n";
        return false;
    }

    s_guest_base = static_cast<u8*>(ptr);
    std::cout << "[Memory] Initialized 4GB Guest Address Space at: " << static_cast<void*>(s_guest_base) << "\n";
    std::cout << "[Memory] Main RAM: " << (MAIN_RAM_SIZE / (1024 * 1024)) << " MB, RSX VRAM: "
              << (RSX_VRAM_SIZE / (1024 * 1024)) << " MB\n";

    return true;
}

void shutdown() {
    if (s_guest_base != nullptr) {
        munmap(s_guest_base, GUEST_ADDR_SPACE_SIZE);
        s_guest_base = nullptr;
        std::cout << "[Memory] Guest Address Space unmapped.\n";
    }
}

u8* translate(GuestAddr addr) {
    return s_guest_base + addr;
}

void copy_to_guest(GuestAddr dst, const void* src, size_t size) {
    std::memcpy(translate(dst), src, size);
}

void copy_from_guest(void* dst, GuestAddr src, size_t size) {
    std::memcpy(dst, translate(src), size);
}

u8* get_base_ptr() {
    return s_guest_base;
}

} // namespace tokisaki::memory
