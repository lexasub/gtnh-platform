#include "Storage/ChunkStore.h"
#include "Network/ChunkStoreService.h"
#include "Network/RouterClient.h"
#include <spdlog/spdlog.h>
#include <csignal>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <string_view>
#include <iostream>
#include <string>
#include <chrono>  // For steady_clock and time calculations

// ── Global state for signal handling ──────────────────────────────────────
static std::atomic<bool> g_stop{false};          // Shutdown flag
static std::atomic<bool> g_print_metrics{false}; // Metrics request flag

// ── Signal handlers ───────────────────────────────────────────────────────
static void handleSignal(int) { 
    g_stop.store(true, std::memory_order_release); 
}

extern "C" void handleSIGUSR1([[maybe_unused]] int sig) {
    g_print_metrics.store(true, std::memory_order_release);
}

int main(int argc, char* argv[]) {
    // ── Early version check (before any initialization) ──────────────────
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            std::cout << "ChunkStore Service (chunkd)\n";
            std::cout << "Version: (not configured - see main.cpp for setup instructions)\n";
            std::cout << "Git Hash: (not configured)\n";
            std::cout << "Build Date: (not configured)\n";
            return 0;
        }
    }

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
    std::signal(SIGUSR1, handleSIGUSR1);  // Register metrics handler

    // Record startup time for uptime calculation
    const auto start_time = std::chrono::steady_clock::now();

    ServerWorld world;
    world.Init(0, db_path, 2048, db_max_size_mb * 1024ULL * 1024ULL);

    ChunkStoreService tcp_service(world, tcp_port);
    auto router = std::make_shared<RouterClient>(world);

    router->connect(router_host, router_port);
    std::thread router_thread([router] { router->run(); });

    tcp_service.start(); // Запускает сервис в пуле worker-потоков

    // Основной поток ждет сигнала остановки
    while (!g_stop.load()) {
        // Check for metrics request
        if (g_print_metrics.load(std::memory_order_acquire)) {
            g_print_metrics.store(false, std::memory_order_release);
            
            // Calculate uptime
            auto now = std::chrono::steady_clock::now();
            auto uptime_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                now - start_time).count();
            int days = uptime_seconds / 86400;
            int hours = (uptime_seconds % 86400) / 3600;
            int minutes = (uptime_seconds % 3600) / 60;
            int seconds = uptime_seconds % 60;
            
            spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
            spdlog::info("METRICS: ChunkStore Service (chunkd)");
            spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
            spdlog::info("Uptime: {} days, {:02d}:{:02d}:{:02d}", days, hours, minutes, seconds);
            spdlog::info("Database Path: {}", db_path);
            spdlog::info("TCP Port: {}", tcp_port);
            spdlog::info("Router: {}:{}", router_host, router_port);
            spdlog::info("Max DB Size: {} MB", db_max_size_mb);
            spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Останавливаем сервисы в обратном порядке запуска
    spdlog::info("Получен сигнал остановки, начинаем завершение работы...");
    router->stop();
    if (router_thread.joinable()) router_thread.join();
    router.reset();
    
    tcp_service.stop(); // Это остановит worker-потоки и позволит main завершиться

    spdlog::info("ChunkStore shutdown complete");
    return 0;
}