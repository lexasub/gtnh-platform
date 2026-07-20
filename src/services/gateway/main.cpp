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

static std::atomic<bool> g_running{true};

extern "C" void handleSignal([[maybe_unused]] int sig) {
    g_running.store(false, std::memory_order_release);
}

int main(int argc, char* argv[]) {
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

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::signal(SIGPIPE, SIG_IGN);

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
    gateway.subscribe("player.position.load");
    gateway.subscribe("quest.completed");
    gateway.subscribe("quest.unlocked");
    gateway.subscribe("quest.progress.updated");

    spdlog::info("Gateway running — worker thread handles io_uring");

    while (g_running) {
        static auto lastHb = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (now - lastHb >= std::chrono::seconds(20)) {
            lastHb = now;
            gateway.sendHeartbeat();
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Explicit shutdown — calls publish_player_left() before router disconnect
    // to persist last player position.  Relying on the destructor alone would
    // skip the publish because ~IoUringConnection polls the thread for 50 ms
    // after the router is already disconnected.
    gateway.shutdown();
    spdlog::info("Gateway stopped");
    return 0;
}
