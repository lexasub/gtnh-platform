#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <signal.h>
#include <atomic>
#include <thread>
#include <cstdlib>
#include <string>
#include <string>

#include "../../libs/libgtnh-common/metrics_util.h"

#include "Client/MessageRouterClient.h"
#include "RecipeManagerService.h"
#include <recipe_manager_lib/RecipeManager.h>
#include <recipe_manager_lib/ItemRegistry.h>

using namespace gtnh::recipe_manager;

static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running.store(false, std::memory_order_release);
}

static std::string getDataDir() {
    const char* env = std::getenv("RECIPED_DATA_DIR");
    if (env) return env;
    return DATA_DIR;
}

int main(int argc, char** argv) {
    gtnh::metrics::printVersionAndExit("RecipeManager Service (reciped)", argc, argv);

    gtnh::metrics::Collector metrics;
    metrics.install();

    spdlog::set_default_logger(spdlog::stdout_color_mt("reciped"));

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

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
        if (metrics.poll()) {
            metrics.printMetrics("RecipeManager Service (reciped)",
                std::string("Recipe Count: ") + std::to_string(recipes->recipeCount()));
        }
        
        ioCtx.poll_one();
        if (!g_running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    router.Disconnect();
    spdlog::info("RecipeManager shutting down");
    return 0;
}
