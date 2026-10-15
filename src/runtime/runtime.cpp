#include "runtime.hpp"
#include "memory.hpp"
#include <iostream>

namespace tokisaki {

Runtime::Runtime() : spu_mgr_(std::make_unique<spu::SPUManager>()) {}

Runtime::~Runtime() {
    shutdown();
}

bool Runtime::initialize(const RuntimeConfig& config) {
    config_ = config;

    std::cout << "========================================================\n";
    std::cout << "  Tokisaki Port - " << config_.game_title << " [" << config_.title_id << "]\n";
    std::cout << "  PS3 Static Recompilation Runtime Core\n";
    std::cout << "========================================================\n";

    if (!memory::initialize()) {
        std::cerr << "[Runtime] Fatal: Failed to initialize memory subsystem.\n";
        return false;
    }

    if (!spu_mgr_->initialize(config_.num_spu_threads)) {
        std::cerr << "[Runtime] Fatal: Failed to initialize SPU manager.\n";
        return false;
    }

    running_ = true;
    std::cout << "[Runtime] Subsystems initialized successfully.\n";
    return true;
}

void Runtime::run() {
    if (!running_) {
        std::cerr << "[Runtime] Cannot run uninitialized runtime.\n";
        return;
    }

    std::cout << "[Runtime] Dispatching main guest execution context...\n";
    std::cout << "[Runtime] Awaiting recompiled binary hooks / ELF payload...\n";
}

void Runtime::shutdown() {
    if (!running_) {
        return;
    }

    std::cout << "[Runtime] Shutting down Tokisaki Port runtime...\n";
    if (spu_mgr_) {
        spu_mgr_->shutdown();
    }
    memory::shutdown();
    running_ = false;
    std::cout << "[Runtime] Shutdown completed.\n";
}

} // namespace tokisaki
