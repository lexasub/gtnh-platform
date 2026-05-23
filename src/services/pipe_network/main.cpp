#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <signal.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdlib>

#include "Client/MessageRouterClient.h"
#include "PipeNetworkService.h"

using namespace gtnh::pipe_network;

static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

int main(int argc, char** argv) {
    spdlog::set_default_logger(spdlog::stdout_color_mt("pipe_networkd"));

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

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
        ioCtx.poll_one();
        if (!g_running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    service.Stop();
    router.Disconnect();
    spdlog::info("PipeNetwork service shutting down");
    return 0;
}
