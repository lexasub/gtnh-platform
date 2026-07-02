#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <signal.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <iostream>
#include <string>

#include "Client/MessageRouterClient.h"
#include "RecipeManagerService.h"
#include <recipe_manager_lib/RecipeManager.h>
#include <recipe_manager_lib/ItemRegistry.h>

using namespace gtnh::recipe_manager;

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_print_metrics{false};

static void signalHandler(int) {
    g_running.store(false, std::memory_order_release);
}

extern "C" void handleSIGUSR1([[maybe_unused]] int sig) {
    g_print_metrics.store(true, std::memory_order_release);
}

static std::string getDataDir() {
    const char* env = std::getenv("RECIPED_DATA_DIR");
    if (env) return env;
    return DATA_DIR;
}

int main(int argc, char** argv) {
    // ── Early version check (before any initialization) ──────────────────
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--version" || arg == "-v") {
            std::cout << "RecipeManager Service (reciped)\n";
            std::cout << "Version: (not configured - see main.cpp for setup instructions)\n";
            std::cout << "Git Hash: (not configured)\n";
            std::cout << "Build Date: (not configured)\n";
            return 0;
        }
    }

    spdlog::set_default_logger(spdlog::stdout_color_mt("reciped"));

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGUSR1, handleSIGUSR1);

    const auto start_time = std::chrono::steady_clock::now();

    std::string routerHost = "127.0.0.1";
    uint16_t routerPort = 5555;
    std::string dataDir = getDataDir();

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--router-host" && i + 1 < argc) {
            routerHost = argv[++i];
        } else if (arg == "--router-port" && i + 1 < argc) {
            routerPort = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--data-dir" && i + 1 < argc) {
            dataDir = argv[++i];
        }
    }

    spdlog::info("RecipeManager starting, router {}:{}, data {}", routerHost, routerPort, dataDir);

    // Pre-load item registry from data dir (overrides lazy-init in RecipeManager)
    RecipeManager::ItemRegistry::instance().loadFromCSV(dataDir + "/data/registry/items.csv");

    auto recipes = std::make_shared<RecipeManager::RecipeManager>();

    // Load machine definitions from YAML (class→variant mapping)
    if (recipes->loadMachinesFromYaml(dataDir + "/data/registry/machines.yaml")) {
        spdlog::info("Loaded machine classes from machines.yaml");
    } else {
        spdlog::warn("Failed to load machines.yaml — YAML recipes will be skipped");
    }

    // Load YAML recipes (tier-aware, class-based)
    if (recipes->loadRecipesFromYamlDirectory(dataDir + "/data/recipes/")) {
        spdlog::info("Total recipes after YAML load: {}", recipes->recipeCount());
    }

    asio::io_context ioCtx;
    gtnh::pipenet::MessageRouterClient router(ioCtx);
    router.SetServiceName("recipe_manager");
    RecipeManagerService service(router, ioCtx, recipes);

    service.Start();
    router.Connect(routerHost, routerPort);

    spdlog::info("RecipeManager ready");

    while (g_running) {
        if (g_print_metrics.load(std::memory_order_acquire)) {
            g_print_metrics.store(false, std::memory_order_release);
            
            auto now = std::chrono::steady_clock::now();
            auto uptime_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                now - start_time).count();
            int days = uptime_seconds / 86400;
            int hours = (uptime_seconds % 86400) / 3600;
            int minutes = (uptime_seconds % 3600) / 60;
            int seconds = uptime_seconds % 60;
            
            spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
            spdlog::info("METRICS: RecipeManager Service (reciped)");
            spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
            spdlog::info("Uptime: {} days, {:02d}:{:02d}:{:02d}", days, hours, minutes, seconds);
            spdlog::info("Router: {}:{}", routerHost, routerPort);
            spdlog::info("Data Directory: {}", dataDir);
            spdlog::info("Recipe Count: {}", recipes->recipeCount());
            spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        }
        
        ioCtx.poll_one();
        if (!g_running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    router.Disconnect();
    spdlog::info("RecipeManager shutting down");
    return 0;
}
