#include <asio.hpp>
#include <spdlog/spdlog.h>
#include <csignal>
#include <atomic>
#include <memory>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <vector>
#include <array>

#include "Network/clients/IoUringRouterClient.h"
#include "Network/clients/IoUringChunkClient.h"
#include "Network/clients/EntityStateStoreClient.h"
#include "Network/RouterEventPublisher.h"
#include "Network/PipeEnergyClient.h"
#include "Network/TopicDispatcher.h"
#include "ECS/Reactors/EnergyFlowHandler.h"
#include "ECS/Reactors/FluidFlowHandler.h"
#include "Crafting/CraftRequestHandler.h"
#include "Crafting/RecipeCompletedHandler.h"
#include "Actions/MachineSlotHandler.h"
#include "Actions/ToolActionHandler.h"
#include "Actions/MiningCalculator.h"
#include "ECS/components/ItemEnergyStorage.h"
#include "Storage/InventoryLoadHandler.h"
#include "Storage/InventoryActionHandler.h"
#include "Storage/PlayerJoinedHandler.h"
#include "Storage/ChunkStoreRepository.h"
#include "Storage/PlayerInventoryStore.h"
#include "Storage/InventorySerializer.h"
#include "ECS/SimulationEngine.h"
#include "World/ChunkEventHandler.h"
#include "Actions/SetBlockCASHandler.h"
#include "Actions/ActionDispatcher.h"
#include "core_generated.h"
#include "meta_db_generated.h"
#include "machine_state_generated.h"
#include "recipe_generated.h"
#include "pipe_network_generated.h"
#include "World/WorldContainerInventory.h"
#include "RecipeManager/RecipeManager.h"
#include "ECS/Systems/MachineSystem.h"
#include "ECS/Systems/GeneratorSystem.h"
#include "ECS/Systems/HeatTransferSystem.h"
#include "ECS/Systems/BoilerSystem.h"
#include "ECS/Systems/CreativeGeneratorSystem.h"
#include "ECS/Systems/BatteryBufferSystem.h"
#include "ECS/Systems/TransformerSystem.h"
#include "ECS/Systems/DrillSystem.h"
#include "ECS/Systems/ExplosionSystem.h"
#include "Network/FluidClient.h"
#include "Actions/WrenchHandler.h"
#include "Actions/MiningCalculator.h"
#include "ECS/components/ItemEnergyStorage.h"
#include "../../data/registry/ToolIds.h"
#include "Crafting/WorkbenchStateManager.h"
#include "MachineRegistry.h"

#include <mutex>
#include <future>
#include <cstdint>

// ── Signal handling ────────────────────────────────────────────────────────
// File-scope statics are acceptable here — std::signal requires function pointers.
static std::atomic<bool>  g_stop{false};
static asio::io_context*  g_io  = nullptr;

static void handleSignal(int sig) {
    spdlog::info("Signal {} received, shutting down...", sig);
    g_stop.store(true);
    if (g_io) g_io->stop();
}

// ── Topic handler helpers ──────────────────────────────────────────────────

namespace {

/// Find a machine entity at the given world position (thread-safe read-only).
entt::entity findEntityAt(const entt::registry& reg, int32_t x, int32_t y, int32_t z) {
    auto view = reg.view<const simcore::Position>();
    for (auto e : view) {
        auto& pp = view.get<const simcore::Position>(e);
        if (static_cast<int32_t>(pp.x) == x &&
            static_cast<int32_t>(pp.y) == y &&
            static_cast<int32_t>(pp.z) == z) return e;
    }
    return entt::null;
}

} // anonymous namespace

// =========================================================================
//  main — composition root
// =========================================================================

int main(int argc, char* argv[]) {
    const char*  router_host     = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t     router_port     = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : 4000;
    const char*  chunkstore_host = (argc > 3) ? argv[3] : "127.0.0.1";
    uint16_t     chunkstore_port = (argc > 4) ? static_cast<uint16_t>(std::atoi(argv[4])) : 5001;
    const char*  recipes_dir     = (argc > 5) ? argv[5] : "/home/su/src/local/gtnh-platform/data/recipes";
    const char*  machines_yaml   = (argc > 6) ? argv[6] : "data/registry/machines.yaml";

    spdlog::set_level(spdlog::level::debug);
    spdlog::info("Starting SimulationCore...");

    // ── Machine registry (from YAML) ──────────────────────────────────────
    auto machineRegistry = MachineRegistry::LoadFromYaml(machines_yaml);
    if (machineRegistry) {
        MachineRegistry::setInstance(machineRegistry.get());
        spdlog::info("Loaded {} machine types from registry", machineRegistry->All().size());
    }

    // ── Recipe manager ────────────────────────────────────────────────────
    auto recipeManager = std::make_shared<RecipeManager::RecipeManager>();
    if (recipeManager->loadRecipesFromDirectory(recipes_dir)) {
        spdlog::info("Loaded {} recipes from {}", recipeManager->recipeCount(), recipes_dir);
    } else {
        spdlog::warn("Failed to load recipes from {}", recipes_dir);
    }

    // ── Signals ───────────────────────────────────────────────────────────
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT,  handleSignal);
    std::signal(SIGTERM, handleSignal);

    // ── Network clients ───────────────────────────────────────────────────
    auto routerClient     = std::make_shared<simcore::IoUringRouterClient>();
    auto chunkstoreClient = std::make_shared<simcore::IoUringChunkClient>();

    // ── asio io_context (EntityStateStore) ────────────────────────────────
    asio::io_context io;
    g_io = &io;

    // ── Entity state storage (uses asio, needed for machine persistence) ───
    auto entityStateClient = std::make_shared<simcore::EntityStateStoreClient>(io);
    entityStateClient->Connect("127.0.0.1", 5200);

    // ── Domain services (constructed explicitly — DI by hand) ─────────────
    auto blockRepository    = std::make_shared<simcore::ChunkStoreRepository>(chunkstoreClient);
    auto eventPublisher     = std::make_shared<simcore::RouterEventPublisher>(routerClient);
    auto pipeEnergyClient   = std::make_shared<simcore::PipeEnergyClient>(routerClient);
    auto fluidClient        = std::make_shared<simcore::FluidClient>(routerClient);
    auto inventoryStore     = std::make_shared<simcore::PlayerInventoryStore>();
    inventoryStore->setOnChange([routerClient](uint64_t player_id, uint16_t slot_index,
                                                uint16_t item_id, uint8_t count, uint16_t meta) {
        flatbuffers::FlatBufferBuilder fb(64);
        auto req = Protocol::CreateSetInventorySlotReq(fb, player_id, slot_index, item_id, count, meta);
        auto msg = Protocol::CreateMetaDBMessage(fb, 0, Protocol::MetaDBRequest_SetInventorySlotReq, req.Union());
        auto frame = Protocol::CreateMetaDBFrame(fb, Protocol::MetaDBPayload_MetaDBMessage, msg.Union());
        fb.Finish(frame);
        std::vector<uint8_t> buf(fb.GetBufferPointer(), fb.GetBufferPointer() + fb.GetSize());
        routerClient->Publish("meta_db.inventory.set", std::move(buf));
    });
    inventoryStore->setPostMutation([routerClient, inventoryStore](uint64_t player_id, const std::array<simcore::PersistSlot, simcore::kInventorySlots>&) {
        flatbuffers::FlatBufferBuilder fb(512);
        auto update = inventoryStore->buildUpdate(fb, player_id);
        fb.Finish(update);
        std::vector<uint8_t> buf(fb.GetBufferPointer(), fb.GetBufferPointer() + fb.GetSize());
        routerClient->Publish("player.inventory.update", std::move(buf));
    });
    auto simulationEngine   = std::make_shared<simcore::SimulationEngine>();
    if (machineRegistry) {
        simulationEngine->setMachineRegistry(machineRegistry.get());
    }

    simulationEngine->onMachineCreated = [eventPublisher, &simulationEngine, entityStateClient](
        int32_t x, int32_t y, int32_t z, uint16_t machine_id) {
        eventPublisher->publishBlockEntityUpdate(x, y, z, machine_id, {}, 0.0f, 0);

        // Try to restore previously saved machine state
        auto entity = findEntityAt(simulationEngine->reg(), x, y, z);
        if (entity != entt::null) {
            entityStateClient->LoadEntityState(0, x, y, z, machine_id,
                [&reg = simulationEngine->reg(), x, y, z](const simcore::EntityStateStoreClient::EntityStateData& state) {
                    if (state.state.empty()) return;
                    auto verifier = flatbuffers::Verifier(state.state.data(), state.state.size());
                    if (!verifier.VerifyBuffer<Protocol::MachineState>(nullptr)) return;
                    auto fbState = flatbuffers::GetRoot<Protocol::MachineState>(state.state.data());
                    auto* inv = fbState->inventory();
                    if (!inv || !inv->slots()) return;

                    auto entity2 = findEntityAt(reg, x, y, z);
                    if (entity2 == entt::null) return;
                    auto* container = reg.try_get<simcore::InventoryContainer>(entity2);
                    if (!container) return;

                    container->slots.clear();
                    for (size_t i = 0; i < inv->slots()->size(); ++i) {
                        auto* s = inv->slots()->Get(i);
                        container->slots.emplace_back(s->item_id(),
                            static_cast<uint8_t>(s->count()), s->meta());
                    }
                    spdlog::debug("[SimCore] Restored saved state at ({},{},{})", x, y, z);
                });
        }

        spdlog::debug("Published BlockEntityUpdate for new multiblock at ({},{},{}) type={}",
                       x, y, z, machine_id);
    };

    // ── ECS Systems ───────────────────────────────────────────────────────
    if (machineRegistry) {
        auto hts = std::make_unique<simcore::HeatTransferSystem>(
            simulationEngine->reg(), *machineRegistry, eventPublisher);
        simulationEngine->registerSystem(std::move(hts));
    }
    {
        auto es = std::make_unique<simcore::ExplosionSystem>(
            simulationEngine->reg(), eventPublisher);
        simulationEngine->registerSystem(std::move(es));
    }
    simcore::MachineSystem* machineSystemRaw = nullptr;
    {
        auto ms = std::make_unique<simcore::MachineSystem>(
            simulationEngine->reg(), recipeManager, eventPublisher, pipeEnergyClient);
        machineSystemRaw = ms.get();
        simulationEngine->registerSystem(std::move(ms));
    }
    {
        auto gs = std::make_unique<simcore::GeneratorSystem>(
            simulationEngine->reg(), eventPublisher, pipeEnergyClient);
        simulationEngine->registerSystem(std::move(gs));
    }
    {
        auto cgs = std::make_unique<simcore::CreativeGeneratorSystem>(
            simulationEngine->reg(), eventPublisher, pipeEnergyClient);
        simulationEngine->registerSystem(std::move(cgs));
    }
    {
        auto bs = std::make_unique<simcore::BoilerSystem>(
            simulationEngine->reg(), eventPublisher, pipeEnergyClient);
        simulationEngine->registerSystem(std::move(bs));
    }
    simcore::BatteryBufferSystem* batteryBufferRaw = nullptr;
    {
        auto bbs = std::make_unique<simcore::BatteryBufferSystem>(
            simulationEngine->reg(), pipeEnergyClient);
        batteryBufferRaw = bbs.get();
        simulationEngine->registerSystem(std::move(bbs));
    }
    {
        auto ts = std::make_unique<simcore::TransformerSystem>(
            simulationEngine->reg(), eventPublisher, pipeEnergyClient);
        simulationEngine->registerSystem(std::move(ts));
    }
    {
        auto ds = std::make_unique<simcore::DrillSystem>(
            simulationEngine->reg(), blockRepository, eventPublisher, pipeEnergyClient);
        simulationEngine->registerSystem(std::move(ds));
    }

    // ── Topic-based message dispatch (O(1) unordered_map, no if-else chain) ──
    auto topicDispatcher = std::make_shared<simcore::TopicDispatcher>();

    // Energy/fluid flow
    topicDispatcher->on("energy.flow", std::make_unique<simcore::EnergyFlowHandler>(
        simulationEngine->reg(), pipeEnergyClient));
    topicDispatcher->on("fluid.flow", std::make_unique<simcore::FluidFlowHandler>(
        simulationEngine->reg(), fluidClient));

    // Crafting
    topicDispatcher->on("sim.craft.request", std::make_unique<simcore::CraftRequestHandler>(
        routerClient, recipeManager, inventoryStore));

    // Recipe
    topicDispatcher->on("recipe.completed", std::make_unique<simcore::RecipeCompletedHandler>(
        simulationEngine));

    // Machine slots
    topicDispatcher->on("player.machine.slot", std::make_unique<simcore::MachineSlotHandler>(
        simulationEngine, inventoryStore, entityStateClient, eventPublisher, routerClient));

    // Tool actions
    topicDispatcher->on("player.tool.action", std::make_unique<simcore::ToolActionHandler>(
        simulationEngine, inventoryStore, routerClient));

    // Inventory
    topicDispatcher->on("player.inventory.load", std::make_unique<simcore::InventoryLoadHandler>(
        inventoryStore, routerClient));
    topicDispatcher->on("player.inventory.actions", std::make_unique<simcore::InventoryActionHandler>(
        inventoryStore, routerClient));
    topicDispatcher->on("player.joined", std::make_unique<simcore::PlayerJoinedHandler>(
        inventoryStore));

    // ── Action handlers ───────────────────────────────────────────────────
    auto casHandler = std::make_shared<simcore::SetBlockCASHandler>(
        blockRepository, eventPublisher, simulationEngine,
    [inventoryStore](uint64_t player_id, uint16_t item_id, uint8_t count, int32_t target_slot) {
        inventoryStore->giveItem(player_id, item_id, count, target_slot);
    },
    [inventoryStore](uint64_t player_id, int32_t x, int32_t y, int32_t z, uint16_t block_id) {
        auto slots = inventoryStore->getSlots(player_id);
        for (int i = 0; i < 9; i++) {
            uint16_t toolId = slots[i].item_id;
            uint8_t tier = toolTier(toolId);
            if (tier == 0 && toolId != ITEM_DRILL_ULV) continue;
            int32_t energyCost = miningEnergyCost(toolId, block_id);
            simulation_core::ItemStack stack{toolId, slots[i].count, slots[i].meta};
            if (consumeToolEnergy(stack, energyCost)) {
                slots[i].meta = stack.meta;
                inventoryStore->setSlots(player_id, slots);
                spdlog::info("[Drill] player {} used {} at ({},{},{}) cost={} remaining={}",
                             player_id, toolId, x, y, z, energyCost, stack.meta);
            }
            break;
        }
    });
simcore::ActionDispatcher dispatcher(casHandler,
    [inventoryStore](uint64_t player_id, uint16_t item_id, uint8_t count, int32_t target_slot) {
        inventoryStore->giveItem(player_id, item_id, count, target_slot);
    });

    simcore::ChunkEventHandler chunkHandler(simulationEngine);

    // ── Crafting ──────────────────────────────────────────────────────────
    auto wbStateManager = std::make_shared<simulation_core::WorkbenchStateManager>(
        entityStateClient, 0 /* dimension */);

    simcore::WorldContainerInventory worldContainers(
        simulationEngine->reg(), entityStateClient);

    // ── Router message handler (composition root — wires topics to services) ──
    routerClient->SetServiceName("simcore");
    routerClient->OnMessage([&](const std::string& topic, const std::vector<uint8_t>& data) {
        if (topic == "player.actions") {
            dispatcher.dispatch(data);
        } else if (topic == "world.blocks.changed") {
            chunkHandler.handle(data);
        } else if (topic == "energy.consume.response") {
            auto* resp = flatbuffers::GetRoot<Protocol::EnergyConsumeResp>(data.data());
            if (!resp) return;
            auto consumed = resp->consumed();
            auto remaining = resp->remaining();
            if (!batteryBufferRaw || !batteryBufferRaw->onConsumeResponse(0, consumed, remaining)) {
                machineSystemRaw->onConsumeResponse(consumed, remaining);
            }

        } else if (topic == "fluid.consume.response") {
            auto* resp = flatbuffers::GetRoot<Protocol::FluidConsumeResp>(data.data());
            if (!resp) return;
            spdlog::trace("FluidConsumeResp: consumed={} remaining={}",
                           resp->consumed(), resp->remaining());

        } else if (topic == "player.chest.open") {
            flatbuffers::Verifier v(data.data(), data.size());
            if (!v.VerifyBuffer<Protocol::ChestOpenReq>(nullptr)) return;
            auto* req = flatbuffers::GetRoot<Protocol::ChestOpenReq>(data.data());
            if (!req || !req->pos()) return;
            auto* p = req->pos();
            int32_t x = p->x(), y = p->y(), z = p->z();

            if (req->open()) {
                entityStateClient->LoadEntityState(0, x, y, z, 3,
                    [routerClient, x, y, z](const simcore::EntityStateStoreClient::EntityStateData& state) {
                        flatbuffers::FlatBufferBuilder fbb(512);
                        auto posFb = Protocol::Vec3i(x, y, z);
                        std::vector<flatbuffers::Offset<Protocol::InventorySlot>> slotOffsets;
                        if (!state.state.empty()) {
                            auto verifier = flatbuffers::Verifier(state.state.data(), state.state.size());
                            if (verifier.VerifyBuffer<Protocol::MachineState>(nullptr)) {
                                auto fbState = flatbuffers::GetRoot<Protocol::MachineState>(state.state.data());
                                auto* inv = fbState->inventory();
                                if (inv && inv->slots()) {
                                    for (size_t i = 0; i < inv->slots()->size(); ++i) {
                                        auto* s = inv->slots()->Get(i);
                                        slotOffsets.push_back(Protocol::CreateInventorySlot(fbb, s->item_id(), static_cast<uint8_t>(s->count()), s->meta()));
                                    }
                                }
                            }
                        }
                        for (int i = static_cast<int>(slotOffsets.size()); i < 27; ++i)
                            slotOffsets.push_back(Protocol::CreateInventorySlot(fbb, 0, 0, 0));
                        auto slots = fbb.CreateVector(slotOffsets);
                        auto resp = Protocol::CreateChestOpenResp(fbb, &posFb, true, slots);
                        fbb.Finish(resp);
                        std::vector<uint8_t> rd(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
                        routerClient->Publish("player.chest.open.response", std::move(rd));
                    });
            } else {
                worldContainers.onContainerClose(req->player_id(), x, y, z);
                flatbuffers::FlatBufferBuilder fbb(128);
                auto posFb = Protocol::Vec3i(x, y, z);
                auto resp = Protocol::CreateChestOpenResp(fbb, &posFb, true, 0);
                fbb.Finish(resp);
                std::vector<uint8_t> rd(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
                routerClient->Publish("player.chest.open.response", std::move(rd));
            }

        } else if (topicDispatcher->dispatch(topic, data)) {
            // Handled by TopicDispatcher — see registration above
        } else {
            spdlog::debug("Unhandled topic: {}", topic);
        }
    });

    // ── Connect & subscribe ───────────────────────────────────────────────
    routerClient->Connect(router_host, router_port);
    chunkstoreClient->Connect(chunkstore_host, chunkstore_port);

    // TopicDispatcher registers its own subscriptions
    topicDispatcher->subscribeAll(routerClient);

    // Remaining subscriptions (not handled by TopicDispatcher)
    routerClient->Subscribe("player.actions");
    routerClient->Subscribe("world.blocks.changed");
    routerClient->Subscribe("fluid.consume.response");
    routerClient->Subscribe("player.chest.open");

    spdlog::info("SimulationCore running, waiting for messages...");

    // ── Main loop ─────────────────────────────────────────────────────────
    std::thread ioThread([&io]() { io.run(); });

    constexpr auto TICK_INTERVAL = std::chrono::milliseconds(50);
    auto nextTick = std::chrono::steady_clock::now();
    auto lastHb   = std::chrono::steady_clock::now();

    while (!g_stop) {
        auto now = std::chrono::steady_clock::now();

        if (now - lastHb >= std::chrono::seconds(20)) {
            lastHb = now;
            routerClient->SendHeartbeat();
        }
        if (now >= nextTick) {
            simulationEngine->tickAll(1.0f);
            nextTick += TICK_INTERVAL;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    io.stop();
    ioThread.join();

    // ── Shutdown (reverse order of construction) ──────────────────────────
    spdlog::info("Shutting down SimulationCore...");
    routerClient->Stop();
    chunkstoreClient->Disconnect();
    entityStateClient->Disconnect();
    spdlog::info("SimulationCore shutdown complete");
    return 0;
}
