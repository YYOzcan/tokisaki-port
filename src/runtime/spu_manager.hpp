#pragma once

#include "common/types.hpp"
#include <array>
#include <thread>
#include <atomic>
#include <vector>
#include <string>

namespace tokisaki::spu {

constexpr size_t SPU_LOCAL_STORE_SIZE = 256 * 1024; // 256 KB Local Storage
constexpr size_t NUM_USER_SPUS = 6;                // PS3 exposes 6 SPUs to game applications

struct SPUContext {
    u32 id{0};
    std::string name;
    alignas(16) u8 local_store[SPU_LOCAL_STORE_SIZE]{};
    alignas(16) vec128 r[128]{}; // 128 x 128-bit vector registers
    u32 npc{0};                  // Next Program Counter

    // SPU Status & MFC (Memory Flow Controller)
    std::atomic<bool> is_running{false};
    std::atomic<bool> stop_signal{false};

    // Mailbox simulation
    u32 outbound_mb{0};
    u32 inbound_mb{0};
};

class SPUManager {
public:
    SPUManager();
    ~SPUManager();

    bool initialize(size_t num_spus = NUM_USER_SPUS);
    void shutdown();

    SPUContext* get_spu(size_t index);
    size_t get_active_spu_count() const { return spus_.size(); }

    // DMA emulation helper
    void dma_get(SPUContext& spu, u32 ls_addr, GuestAddr ea, u32 size);
    void dma_put(const SPUContext& spu, GuestAddr ea, u32 ls_addr, u32 size);

private:
    std::vector<std::unique_ptr<SPUContext>> spus_;
    std::vector<std::jthread> workers_;
    std::atomic<bool> initialized_{false};
};

} // namespace tokisaki::spu
