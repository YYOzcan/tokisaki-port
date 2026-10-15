#include "spu_manager.hpp"
#include "memory.hpp"
#include <iostream>
#include <cstring>
#include <memory>

namespace tokisaki::spu {

SPUManager::SPUManager() = default;

SPUManager::~SPUManager() {
    shutdown();
}

bool SPUManager::initialize(size_t num_spus) {
    if (initialized_.load()) {
        return true;
    }

    spus_.reserve(num_spus);
    for (size_t i = 0; i < num_spus; ++i) {
        auto spu = std::make_unique<SPUContext>();
        spu->id = static_cast<u32>(i);
        spu->name = "SPU_" + std::to_string(i);
        spus_.push_back(std::move(spu));
    }

    initialized_.store(true);
    std::cout << "[SPU] Initialized " << spus_.size() << " Cell Synergistic Processing Units.\n";
    return true;
}

void SPUManager::shutdown() {
    if (!initialized_.load()) {
        return;
    }

    for (auto& spu : spus_) {
        spu->stop_signal.store(true);
    }

    workers_.clear();
    spus_.clear();
    initialized_.store(false);
    std::cout << "[SPU] All SPUs stopped and cleaned up.\n";
}

SPUContext* SPUManager::get_spu(size_t index) {
    if (index >= spus_.size()) {
        return nullptr;
    }
    return spus_[index].get();
}

void SPUManager::dma_get(SPUContext& spu, u32 ls_addr, GuestAddr ea, u32 size) {
    if (ls_addr + size > SPU_LOCAL_STORE_SIZE) {
        std::cerr << "[SPU DMA GET] Address out of LS bounds: 0x" << std::hex << ls_addr << "\n";
        return;
    }
    const u8* src = memory::translate(ea);
    std::memcpy(&spu.local_store[ls_addr], src, size);
}

void SPUManager::dma_put(const SPUContext& spu, GuestAddr ea, u32 ls_addr, u32 size) {
    if (ls_addr + size > SPU_LOCAL_STORE_SIZE) {
        std::cerr << "[SPU DMA PUT] Address out of LS bounds: 0x" << std::hex << ls_addr << "\n";
        return;
    }
    u8* dst = memory::translate(ea);
    std::memcpy(dst, &spu.local_store[ls_addr], size);
}

} // namespace tokisaki::spu
