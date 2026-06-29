#include "src/validation.h"
#include <spdlog/spdlog.h>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    // ── Early version check (before any initialization) ──────────────────
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            std::cout << "Validation Service (validationd)\n";
            std::cout << "Version: (not configured - see main.cpp for setup instructions)\n";
            std::cout << "Git Hash: (not configured)\n";
            std::cout << "Build Date: (not configured)\n";
            return 0;
        }
    }

    spdlog::set_level(spdlog::level::debug);
    spdlog::info("Validation Service starting...");

    gtnh::validation::ValidationService service;
    if (!service.initialize()) {
        spdlog::error("Failed to initialize ValidationService");
        return 1;
    }

    spdlog::info("Validation Service running");

    service.shutdown();
    spdlog::info("Validation Service shutdown complete");
    return 0;
}
