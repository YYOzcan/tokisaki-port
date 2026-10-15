#include "runtime/runtime.hpp"
#include <iostream>
#include <csignal>

static tokisaki::Runtime g_runtime;

void signal_handler(int sig) {
    std::cout << "\n[Signal] Caught interrupt signal (" << sig << "), initiating clean exit...\n";
    g_runtime.shutdown();
    std::exit(0);
}

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    tokisaki::RuntimeConfig config;
    config.game_title = "Killzone 2";
    config.title_id = "BCES00081";

    if (argc > 1) {
        config.eboot_path = argv[1];
    }

    if (!g_runtime.initialize(config)) {
        std::cerr << "[Main] Failed to start Tokisaki Port runtime.\n";
        return 1;
    }

    g_runtime.run();
    g_runtime.shutdown();

    return 0;
}
