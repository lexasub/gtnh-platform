#include "Storage/ChunkStore.h"
#include "Network/ChunkStoreService.h"
#include "Network/RouterClient.h"
#include <spdlog/spdlog.h>
#include <csignal>
#include <cstdlib>
#include <atomic>
#include <memory>
#include <thread>
#include <string_view>

static std::atomic<bool> g_stop{false};

static void handleSignal(int) { g_stop.store(true); }

int main(int argc, char* argv[]) {
    spdlog::set_level(spdlog::level::debug);
    std::string db_path = (argc > 1) ? argv[1] : "./chunkdb";
    uint16_t tcp_port = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : 5001;
    const char* router_host = (argc > 3) ? argv[3] : "127.0.0.1";
    uint16_t router_port = (argc > 4) ? static_cast<uint16_t>(std::atoi(argv[4])) : 4000;

    // Parse optional --db-max-size-mb (default: 262144 MB = 256 GB)
    size_t db_max_size_mb = 262144;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--db-max-size-mb" && i + 1 < argc) {
            db_max_size_mb = static_cast<size_t>(std::atoll(argv[++i]));
        }
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    ChunkStore store(db_path, 2048, db_max_size_mb * 1024ULL * 1024ULL);

    ChunkStoreService tcp_service(store, tcp_port);
    auto router = std::make_shared<RouterClient>(store);

    router->connect(router_host, router_port);
    std::thread router_thread([router] { router->run(); });

    tcp_service.start();

    // Main loop waiting for stop signal
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Shutdown in reverse order
    spdlog::info("Shutdown signal received, stopping services...");
    router->stop();
    if (router_thread.joinable()) router_thread.join();
    router.reset();

    tcp_service.stop();

    spdlog::info("ChunkStore shutdown complete");
    return 0;
}