#include "src/validation.h"
#include <spdlog/spdlog.h>

int main() {
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
