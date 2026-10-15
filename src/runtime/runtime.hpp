#pragma once

#include "common/types.hpp"
#include "spu_manager.hpp"
#include <string>
#include <memory>

namespace tokisaki {

struct RuntimeConfig {
    std::string game_title{"Killzone 2"};
    std::string title_id{"BCES00081"};
    std::string eboot_path{"EBOOT.ELF"};
    size_t num_spu_threads{6};
    bool enable_debug_log{true};
};

class Runtime {
public:
    Runtime();
    ~Runtime();

    bool initialize(const RuntimeConfig& config);
    void run();
    void shutdown();

    spu::SPUManager& get_spu_manager() { return *spu_mgr_; }
    const RuntimeConfig& get_config() const { return config_; }

private:
    RuntimeConfig config_;
    std::unique_ptr<spu::SPUManager> spu_mgr_;
    bool running_{false};
};

} // namespace tokisaki
