#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <pthread.h>
#include <csignal>
#include <iostream>
#include <string>
#include <atomic>
#include <chrono>
#include "GameClient.h"

static GameClient* g_client = nullptr;
static std::atomic<bool> g_print_metrics{false};

static void signalHandler(int) {
    // Can't safely call spdlog here (may deadlock if signal arrives
    // while main thread holds the log mutex). Just poke the client
    // and let the main loop drain naturally.
    if (g_client) {
        g_client->RequestShutdown();
    }
}

extern "C" void handleSIGUSR1([[maybe_unused]] int sig) {
    g_print_metrics.store(true, std::memory_order_release);
}

int main(int argc, char* argv[]) {
    // ── Early version check (before any initialization) ──────────────────
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            std::cout << "GameClient (gtnh-client)\n";
            std::cout << "Version: (not configured - see main.cpp for setup instructions)\n";
            std::cout << "Git Hash: (not configured)\n";
            std::cout << "Build Date: (not configured)\n";
            return 0;
        }
    }

    auto console = spdlog::stdout_color_mt("game_client");
    spdlog::set_default_logger(console);
    // Set to trace for IoUringClient SQE debugging, warn for normal use
    spdlog::set_level(spdlog::level::info);
    if (auto* env = std::getenv("GTNH_LOG_LEVEL")) {
        std::string level(env);
        if (level == "trace") spdlog::set_level(spdlog::level::trace);
        else if (level == "debug") spdlog::set_level(spdlog::level::debug);
        else if (level == "warn") spdlog::set_level(spdlog::level::warn);
    }
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGUSR1, handleSIGUSR1);

    const auto start_time = std::chrono::steady_clock::now();

    pthread_setname_np(pthread_self(), "ClientMain");

    GameClient client;
    g_client = &client;

    auto shaderDir = std::string("shaders");
    auto width = 1280;
    auto height = 720;
    std::string server_host = "127.0.0.1";
    uint16_t server_port = 7777;
    uint16_t bulk_port = 7778;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--shader-dir" && i+1 < argc) shaderDir = argv[++i];
        else if (std::string(argv[i]) == "--resolution" && i+1 < argc) {
            sscanf(argv[++i], "%dx%d", &width, &height);
        } else if (std::string(argv[i]) == "--host" && i+1 < argc) server_host = argv[++i];
        else if (std::string(argv[i]) == "--port" && i+1 < argc) server_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (std::string(argv[i]) == "--bulk-port" && i+1 < argc) bulk_port = static_cast<uint16_t>(std::atoi(argv[++i]));
    }

    if (!client.Init(shaderDir, width, height, server_host, server_port, bulk_port)) {
        spdlog::error("GameClient initialization failed");
        return 1;
    }

    spdlog::info("GameClient started");
    
    // Note: client.Run() blocks in render loop. For SIGUSR1 to work properly,
    // GameClient::Run() would need to check g_print_metrics periodically.
    // For now, signal handler is registered but metrics won't print during Run().
    // This would require modifying GameClient class to poll the flag.
    
    client.Run();

    return 0;
}