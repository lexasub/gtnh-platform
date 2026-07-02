#include "gateway.h"
#include "gateway_generated.h"

#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <iostream>  // For std::cout to print version information
#include <string>    // For std::string comparison in argv parsing

// ── Global state for signal handling ──────────────────────────────────────
// These variables are accessed from both the main thread and signal handlers,
// so they must be atomic or designed for async-signal-safe access.

static std::atomic<bool> g_running{true};           // Shutdown flag (set by SIGINT/SIGTERM)
static std::atomic<bool> g_print_metrics{false};    // Metrics request flag (set by SIGUSR1)

// ── Signal handlers ───────────────────────────────────────────────────────
// Signal handlers run asynchronously - they can interrupt the program at any point.
// Therefore, they must ONLY do async-signal-safe operations:
//   ✅ Safe: atomic operations, setting flags
//   ❌ Unsafe: malloc, printf, logging, most library functions
//
// The "extern C" linkage is required because signal() expects a C function pointer,
// not a C++ function (which may have name mangling).

extern "C" void handleSignal([[maybe_unused]] int sig) {
    // Set the shutdown flag - main loop checks this and exits gracefully
    g_running.store(false, std::memory_order_release);
}

extern "C" void handleSIGUSR1([[maybe_unused]] int sig) {
    // Just set a flag - the actual printing happens in the main loop
    // This is the safe pattern: signal handler sets flag, main loop handles it
    g_print_metrics.store(true, std::memory_order_release);
}

int main(int argc, char* argv[]) {
    // ── Early version check (before any initialization) ──────────────────
    // This allows `./gatewayd --version` to exit immediately without
    // initializing logging, network, or other subsystems.
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        // Check for both --version and -v flags
        if (arg == "--version" || arg == "-v") {
            // Print service name
            std::cout << "Gateway Service (gatewayd)\n";
            
            // Check if version information is available at compile time
            // Currently, there is NO version.h or CMake-injected version.
            // Explanation of how to add build information:
            //
            // To add proper version tracking, create a CMake-generated header:
            //
            // 1. Create src/version.h.in with placeholders:
            //    #define GTNH_VERSION_MAJOR @PROJECT_VERSION_MAJOR@
            //    #define GTNH_VERSION_MINOR @PROJECT_VERSION_MINOR@
            //    #define GTNH_VERSION_PATCH @PROJECT_VERSION_PATCH@
            //    #define GTNH_GIT_HASH "@GIT_HASH@"
            //    #define GTNH_BUILD_DATE "@BUILD_DATE@"
            //
            // 2. In CMakeLists.txt, add:
            //    project(GTNHPlatform VERSION 0.1.0 CXX C)
            //    execute_process(
            //        COMMAND git rev-parse --short HEAD
            //        OUTPUT_VARIABLE GIT_HASH
            //        OUTPUT_STRIP_TRAILING_WHITESPACE
            //    )
            //    string(TIMESTAMP BUILD_DATE "%Y-%m-%d %H:%M:%S UTC" UTC)
            //    configure_file(src/version.h.in ${CMAKE_BINARY_DIR}/version.h @ONLY)
            //
            // 3. Include version.h in this file and print:
            //    std::cout << "Version: " << GTNH_VERSION_MAJOR << "."
            //              << GTNH_VERSION_MINOR << "." << GTNH_VERSION_PATCH << "\n";
            //    std::cout << "Git Hash: " << GTNH_GIT_HASH << "\n";
            //    std::cout << "Build Date: " << GTNH_BUILD_DATE << "\n";
            
            std::cout << "Version: (not configured - see main.cpp for setup instructions)\n";
            std::cout << "Git Hash: (not configured)\n";
            std::cout << "Build Date: (not configured)\n";
            
            return 0;  // Exit successfully after printing version
        }
    }

    // ── Normal argument parsing (for runtime configuration) ───────────────
    uint16_t router_port = 4000;
    uint16_t ctrl_port = 7777;
    uint16_t bulk_port = 7778;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--router-port" && i + 1 < argc)
            router_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (arg == "--port" && i + 1 < argc)
            ctrl_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (arg == "--bulk-port" && i + 1 < argc)
            bulk_port = static_cast<uint16_t>(std::atoi(argv[++i]));
    }

    auto console = spdlog::stdout_color_mt("gateway");
    spdlog::set_default_logger(console);
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    spdlog::info("Gateway starting: router=localhost:{} ctrl=:{} bulk=:{}",
                 router_port, ctrl_port, bulk_port);

    // ── Register signal handlers ──────────────────────────────────────────
    // std::signal() registers a function to be called when a signal is received
    
    std::signal(SIGINT, handleSignal);   // Ctrl+C in terminal
    std::signal(SIGTERM, handleSignal);  // kill <pid> (graceful shutdown)
    std::signal(SIGPIPE, SIG_IGN);       // Ignore broken pipe (write to closed socket)
    std::signal(SIGUSR1, handleSIGUSR1); // User signal 1 (for metrics on demand)
    //                   ^^^^^^^^^^^^^^
    //                   New handler for metrics printing

    // ── Record startup time ───────────────────────────────────────────────
    // steady_clock is monotonic (doesn't jump if system time changes)
    // We'll use this to calculate uptime when SIGUSR1 is received
    const auto start_time = std::chrono::steady_clock::now();

    IoUringGateway gateway;

    gateway.on_router_message = [&](const std::string& topic,
                                     const uint8_t* data, size_t len) {
        if (topic == "metadb.player.online") {
            gateway.publish_player_joined();
        } else if (topic == "player.actions.ack")
            gateway.send_to_client_ctrl_raw(GatewayMsg::kBlockAck, data, len);
        else if (topic == "player.inventory.update")
            gateway.send_to_client_ctrl_raw(GatewayMsg::kInventoryUpdate, data, len);
        else if (topic == "sim.craft.response")
            gateway.send_to_client_ctrl_raw(GatewayMsg::kCraftResponse, data, len);
        else if (topic == "player.machine.slot.response")
            gateway.send_to_client_ctrl_raw(GatewayMsg::kSetMachineSlotResp, data, len);
        else if (topic == "world.block_entity.update")
            gateway.send_to_client_ctrl_raw(GatewayMsg::kBlockEntityUpdate, data, len);
        else if (topic == "recipe.completed")
            gateway.send_to_client_ctrl_raw(GatewayMsg::kRecipeCompleted, data, len);
        else
            spdlog::trace("Gateway: unhandled topic '{}' ({} bytes)", topic, len);
    };

    gateway.on_client_message = [&](const uint8_t* data, size_t len) {
        //spdlog::info("[GATEWAY] Publishing {} bytes to 'player.actions'", len);
        gateway.publish("player.actions", data, len);
    };

    if (!gateway.init()) {
        spdlog::error("Gateway: failed to init io_uring");
        return 1;
    }

    if (!gateway.listen(ctrl_port, bulk_port)) {
        spdlog::error("Gateway: failed to listen on ports {} {}", ctrl_port, bulk_port);
        return 1;
    }

    if (!gateway.connect_router("127.0.0.1", router_port)) {
        spdlog::error("Gateway: failed to connect to Router");
        return 1;
    }

    gateway.subscribe("metadb.player.online");
    gateway.subscribe("world.chunk.loaded.compressed");
    gateway.subscribe("world.blocks.changed");
    gateway.subscribe("entities.#");
    gateway.subscribe("simulation.multiblock.created");
    gateway.subscribe("simulation.multiblock.destroyed");
    gateway.subscribe("player.actions.ack");
    gateway.subscribe("player.inventory.update");
    gateway.subscribe("sim.craft.response");
    gateway.subscribe("player.machine.slot.response");
    gateway.subscribe("player.tool.action.response");
    gateway.subscribe("world.block_entity.update");
    gateway.subscribe("recipe.completed");

    spdlog::info("Gateway running — worker thread handles io_uring");

    // ── Main event loop ───────────────────────────────────────────────────
    while (g_running) {
        // ── Check for metrics request (SIGUSR1) ───────────────────────────
        // The signal handler just sets the flag; we do the actual work here
        // in the main loop where it's safe to use logging, allocations, etc.
        if (g_print_metrics.load(std::memory_order_acquire)) {
            g_print_metrics.store(false, std::memory_order_release);
            
            // Calculate uptime
            auto now = std::chrono::steady_clock::now();
            auto uptime_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                now - start_time).count();
            
            // Format uptime as days, hours, minutes, seconds
            int days = uptime_seconds / 86400;
            int hours = (uptime_seconds % 86400) / 3600;
            int minutes = (uptime_seconds % 3600) / 60;
            int seconds = uptime_seconds % 60;
            
            // Print metrics using spdlog (safe here in main loop, not in signal handler)
            spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
            spdlog::info("METRICS: Gateway Service (gatewayd)");
            spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
            spdlog::info("Uptime: {} days, {:02d}:{:02d}:{:02d}", days, hours, minutes, seconds);
            spdlog::info("Control Port: {}", ctrl_port);
            spdlog::info("Bulk Port: {}", bulk_port);
            spdlog::info("Router Port: {}", router_port);
            spdlog::info("Client Connected: {}", gateway.has_client() ? "yes" : "no");
            spdlog::info("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
        }
        
        // ── Heartbeat timer ───────────────────────────────────────────────
        static auto lastHb = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (now - lastHb >= std::chrono::seconds(20)) {
            lastHb = now;
            gateway.sendHeartbeat();
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    spdlog::info("Gateway stopped");
    return 0;
}
