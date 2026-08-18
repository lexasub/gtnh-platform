#include "gateway.h"
#include "gateway_generated.h"

#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <string>

#include "../../libs/libgtnh-common/metrics_util.h"

static std::atomic<bool> g_running{true};

extern "C" void handleSignal([[maybe_unused]] int sig) {
    g_running.store(false, std::memory_order_release);
}

int main(int argc, char* argv[]) {
    gtnh::metrics::printVersionAndExit("Gateway Service (gatewayd)", argc, argv);

    gtnh::metrics::Collector metrics;
    metrics.install();

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
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::signal(SIGPIPE, SIG_IGN);

    IoUringGateway gateway;

    gateway.on_router_message = [&](const std::string& topic,
                                     const uint8_t* data, size_t len) {
        // NOTE: Only topics NOT already handled in on_router_publish() should be here.
        // on_router_publish() handles: world.chunk.loaded.compressed, world.blocks.changed,
        // entities.#, player.actions.ack, player.inventory.update, world.block_entity.update,
        // sim.craft.response, player.machine.slot.response, player.tool.action.response,
        // player.position.load, multiblock events, quest topics, meta_db.quest.get.response.
        if (topic == "metadb.player.online") {
            gateway.publish_player_joined();
        } else if (topic == "recipe.completed")
            gateway.send_to_client_ctrl_raw(GatewayMsg::kRecipeCompleted, data, len);
        else
            spdlog::trace("Gateway: unhandled topic '{}' ({} bytes)", topic, len);
    };

    gateway.on_client_message = [&](const uint8_t* data, size_t len) {
        // Filter PlayerActions on the gateway before they reach the router:
        // the client floods MOVE/UNLOAD at ~15k/s while walking (chunk
        // eviction).  Only CHUNK_REQUEST (chunkd) and ITEM_ACTION (simcore)
        // are meaningful — forwarding MOVE/UNLOAD saturates the router and
        // starves player.actions.setblock.
        flatbuffers::Verifier v(data, len);
        if (v.VerifyBuffer<Protocol::PlayerAction>(nullptr)) {
            auto* pa = flatbuffers::GetRoot<Protocol::PlayerAction>(data);
            if (pa) {
                switch (pa->action()) {
                case Protocol::PlayerActionType_CHUNK_REQUEST:
                    gateway.publish("chunk.requests", data, len);
                    return;
                case Protocol::PlayerActionType_ITEM_ACTION:
                    gateway.publish("player.actions", data, len);
                    return;
                default:
                    return; // drop MOVE / UNLOAD / etc.
                }
            }
        }
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
    gateway.subscribe("sim.multiblock.created");
    gateway.subscribe("sim.multiblock.destroyed");
    gateway.subscribe("player.actions.ack");
    gateway.subscribe("player.actions.directive");
    gateway.subscribe("player.inventory.update");
    gateway.subscribe("sim.craft.response");
    gateway.subscribe("sim.workbench.state");
    gateway.subscribe("sim.workbench.load");
    gateway.subscribe("player.machine.slot.response");
    gateway.subscribe("player.tool.action.response");
    gateway.subscribe("world.block_entity.update");
    gateway.subscribe("recipe.completed");
    gateway.subscribe("player.position.load");
    gateway.subscribe("quest.completed.notification");
    gateway.subscribe("quest.unlocked");
    gateway.subscribe("quest.progress.updated");
    gateway.subscribe("quest.era.transition");
    gateway.subscribe("meta_db.quest.get.response");
    gateway.subscribe("quest.exchange.response");
    gateway.subscribe("quest.exchange.cooldown.response");
    gateway.subscribe("player.gamemode.changed");
    gateway.subscribe("player.scenario.start.response");
    gateway.subscribe("recipe.check.response");
    gateway.subscribe("recipe.catalog.response");
    gateway.subscribe("recipe.item.response");
    gateway.subscribe("recipe.machine.response");
    gateway.subscribe("fluid.pipe.state");

    spdlog::info("Gateway running — worker thread handles io_uring");

    // ── Main event loop ───────────────────────────────────────────────────
    static auto lastHb = std::chrono::steady_clock::now();
    static auto lastRouterCheck = std::chrono::steady_clock::now();

    while (g_running) {
        if (metrics.poll()) {
            metrics.printMetrics("Gateway Service (gatewayd)",
                std::string("Client Connected: ") + (gateway.has_client() ? "yes" : "no"));
        }

        auto now = std::chrono::steady_clock::now();
        
        // ── Heartbeat timer ───────────────────────────────────────────────
        if (now - lastHb >= std::chrono::seconds(20)) {
            lastHb = now;
            gateway.sendHeartbeat();
        }

        // ── Router reconnect check ────────────────────────────────────────
        // If the RouterClient dropped (no auto-reconnect), re-establish
        // connection and re-register with stored topic subscriptions.
        if (!gateway.is_router_connected() && now - lastRouterCheck >= std::chrono::seconds(3)) {
            lastRouterCheck = now;
            spdlog::warn("Gateway: router disconnected — attempting reconnect");
            if (gateway.connect_router()) {
                spdlog::info("Gateway: reconnected to router");
            } else {
                spdlog::error("Gateway: router reconnect failed");
            }
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
