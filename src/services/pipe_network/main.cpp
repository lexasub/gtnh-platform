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
#include "PipeNetworkService.h"

using namespace gtnh::pipe_network;

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_print_metrics{false};

static void signalHandler(int) {
    g_running.store(false, std::memory_order_release);
}

extern "C" void handleSIGUSR1([[maybe_unused]] int sig) {
    g_print_metrics.store(true, std::memory_order_release);
}

int main(int argc, char** argv) {
    // ── Early version check (before any initialization) ──────────────────
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--version" || arg == "-v") {
            std::cout << "PipeNetwork Service (pipe_networkd)\n";
            std::cout << "Version: (not configured - see main.cpp for setup instructions)\n";
            std::cout << "Git Hash: (not configured)\n";
            std::cout << "Build Date: (not configured)\n";
            return 0;
        }
    }

    spdlog::set_default_logger(spdlog::stdout_color_mt("pipe_networkd"));

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGUSR1, handleSIGUSR1);

    const auto start_time = std::chrono::steady_clock::now();

    std::string routerHost = "127.0.0.1";
    uint16_t routerPort = 4000;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--router-host" && i + 1 < argc) {
            routerHost = argv[++i];
        } else if (arg == "--router-port" && i + 1 < argc) {
            routerPort = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
    }

    spdlog::info("PipeNetwork service starting, router {}:{}", routerHost, routerPort);

    asio::io_context ioCtx;
    MessageRouterClient router(ioCtx);
    router.SetServiceName("pipe_network");
    PipeNetworkService service(router, ioCtx);

    service.Start();
    router.Connect(routerHost, routerPort);

    spdlog::info("PipeNetwork service ready");

    while (g_running) {
        // Check for metrics request
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
            spdlog::info("METRICS: PipeNetwork Service (pipe_networkd)");
            spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
            spdlog::info("Uptime: {} days, {:02d}:{:02d}:{:02d}", days, hours, minutes, seconds);
            spdlog::info("Router: {}:{}", routerHost, routerPort);
            spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        }
        
        ioCtx.poll_one();
        if (!g_running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    service.Stop();
    router.Disconnect();
    spdlog::info("PipeNetwork service shutting down");
    return 0;
}
