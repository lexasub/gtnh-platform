#include <asio.hpp>
#include <spdlog/spdlog.h>
#include <csignal>
#include <atomic>
#include <memory>
#include <thread>
#include <chrono>
#include <vector>
#include <array>
#include <unordered_map>
#include <iostream>
#include <string>
#include <cstdint>

#include "Network/clients/IoUringRouterClient.h"
#include "Network/clients/IoUringChunkClient.h"
#include "Network/clients/EntityStateStoreClient.h"
#include "Network/RouterEventPublisher.h"
#include "Network/PipeEnergyClient.h"
#include "Network/FluidClient.h"
#include "Network/ItemClient.h"
#include "Network/SimCoreMessageHandler.h"
#include "ECS/SimulationEngine.h"
#include "Storage/ChunkStoreRepository.h"
#include "Storage/PlayerInventoryStore.h"
#include "ECS/Systems/MachineSystem.h"
#include "ECS/Systems/BatteryBufferSystem.h"
#include "ECS/Systems/AdjacencyTransferSystem.h"
#include "ECS/Systems/CreativeGeneratorSystem.h"
#include "ECS/Systems/TransformerSystem.h"
#include "ECS/Systems/DrillSystem.h"
#include "ECS/Systems/RotareGeneratorSystem.h"
#include "ECS/Systems/CoolantSystem.h"
#include "ECS/Systems/ExplosionSystem.h"
#include "ECS/Systems/GeneratorSystem.h"
#include "ECS/Systems/BoilerSystem.h"
#include "ECS/Systems/EBFSystem.h"
#include "ECS/Systems/LargeBoilerSystem.h"
#include "ECS/Systems/LCRSystem.h"
#include "Actions/handTool/WrenchHandler.h"
#include "World/WorldContainerInventory.h"
#include "World/BlockDrops.h"
#include "World/BlockTransforms.h"
#include "Quest/QuestManager.h"
#include "RecipeManager/RecipeManager.h"
#include "ItemRegistry.h"
#include "Crafting/WorkbenchStateManager.h"
#include "Storage/ContainerSession.h"
#include "Storage/ChestStateManager.h"
#include "MachineRegistry.h"
#include "Common/MainThreadQueue.h"
#include "core_generated.h"
#include "meta_db_generated.h"
#include "machine_state_generated.h"
#include "multiblock_state_generated.h"
#include <flatbuffers/flatbuffers.h>

#include "../../libs/libgtnh-common/metrics_util.h"

// ── Signal handling ────────────────────────────────────────────────────────
// File-scope statics are acceptable here — std::signal requires function pointers.
static std::atomic<bool>  g_stop{false};
static asio::io_context*  g_io  = nullptr;

static void handleSignal(int sig) {
    spdlog::info("Signal {} received, shutting down...", sig);
    g_stop.store(true, std::memory_order_release);
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

void spawnECSSystems(std::shared_ptr<simcore::ChunkStoreRepository> blockRepository,
                     std::shared_ptr<simcore::RouterEventPublisher> eventPublisher,
                     std::shared_ptr<simcore::PipeEnergyClient> pipeEnergyClient,
                     std::shared_ptr<simcore::SimulationEngine> simulationEngine) {
    // TODO - may be lazy start - on use
    simulationEngine->registerSystem(std::make_unique<simcore::CoolantSystem>(simulationEngine->reg()));
    simulationEngine->registerSystem(std::make_unique<simcore::ExplosionSystem>(simulationEngine->reg(), eventPublisher));
    simulationEngine->registerSystem(std::make_unique<simcore::GeneratorSystem>(simulationEngine->reg(), eventPublisher, pipeEnergyClient));
    simulationEngine->registerSystem(std::make_unique<simcore::CreativeGeneratorSystem>(simulationEngine->reg(), eventPublisher, pipeEnergyClient));
    simulationEngine->registerSystem(std::make_unique<simcore::BoilerSystem>(simulationEngine->reg(), eventPublisher, pipeEnergyClient));
    simulationEngine->registerSystem(std::make_unique<simcore::TransformerSystem>(simulationEngine->reg(), eventPublisher, pipeEnergyClient));
    simulationEngine->registerSystem(std::make_unique<simcore::DrillSystem>(simulationEngine->reg(), blockRepository, eventPublisher, pipeEnergyClient));
    simulationEngine->registerSystem(std::make_unique<simcore::RotareGeneratorSystem>(simulationEngine->reg(), eventPublisher, pipeEnergyClient));
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    gtnh::metrics::printVersionAndExit("SimulationCore Service (simcored)", argc, argv);

    gtnh::metrics::Collector metrics;
    metrics.install();

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

    // ── Item registry (from items.csv → packed ids) ──────────────────────
    {
        auto& reg = RecipeManager::ItemRegistry::instance();
        if (reg.loadFromCSV("data/registry/items.csv")) {
            spdlog::info("Loaded {} items from items.csv", reg.count());
        } else {
            spdlog::warn("Failed to load items.csv");
        }
    }

    // ── Block drops (from drops.csv → broken block → drop) ───────────────
    {
        auto blockDrops = simcore::BlockDrops::Load("data/registry/drops.csv");
        simcore::BlockDrops::setInstance(blockDrops);
    }

    // ── Block transforms (transforms.csv → placed X on Y → Z) ─────────────
    {
        auto blockTransforms =
            simcore::BlockTransforms::Load("data/registry/transforms.csv");
        simcore::BlockTransforms::setInstance(blockTransforms);
    }

    // ── Recipe manager ────────────────────────────────────────────────────
    auto recipeManager = std::make_shared<RecipeManager::RecipeManager>();
    if (recipeManager->loadRecipesFromYamlDirectory(recipes_dir)) {
        spdlog::info("Loaded {} YAML recipes from {}", recipeManager->recipeCount(), recipes_dir);
    } else {
        spdlog::warn("Failed to load YAML recipes from {}", recipes_dir);
    }
    if (recipeManager->loadMachinesFromYaml(machines_yaml)) {
        spdlog::info("Loaded machine classes from {}", machines_yaml);
    } else {
        spdlog::warn("Failed to load machine classes from {}", machines_yaml);
    }

    // ── Multiblock controllers (runtime registration) ─────────────────────
    // TODO(multiblocks): these blocks are not yet in machines.yaml/items.csv
    // (the registry is being regenerated). Register them at runtime so the
    // multiblock formation path and recipe lookup work end-to-end.
    {
        constexpr uint16_t EBF_CONTROLLER = 1003;
        constexpr uint16_t BOILER_CONTROLLER = 1005;
        constexpr uint16_t LCR_CONTROLLER = 1006;

        if (machineRegistry) {
            auto registerController = [&machineRegistry](uint16_t id, const char* name,
                                                         const char* cls,
                                                         std::optional<EnergyType> energy_in,
                                                         std::optional<EnergyType> energy_out,
                                                         int slots_in = 0) {
                MachineInfo info{};
                info.id = id;
                info.name = name;
                info.machine_class = cls;
                info.energy_in = energy_in;
                info.energy_out = energy_out;
                info.tier = 1;
                info.slots_in = static_cast<int>(slots_in); // items go through hatches / fuel container
                info.slots_out = 0;
                info.capacity = 10000;
                info.maxInput = 32;
                info.maxOutput = 32;
                machineRegistry->Register(info);
            };

            // EBF — HEAT consumer; Large Boiler — STEAM producer (fuel via
            // controller container, so give it 4 fuel slots); LCR — ELECTRICITY.
            registerController(EBF_CONTROLLER, "electric_blast_furnace", "ebf",
                               EnergyType::HEAT, std::nullopt);
            registerController(BOILER_CONTROLLER, "large_boiler", "large_boiler",
                               std::nullopt, EnergyType::STEAM,
                               /*slots_in=*/4);
            registerController(LCR_CONTROLLER, "large_chemical_reactor", "chemical_reactor",
                               EnergyType::ELECTRICITY, std::nullopt);
        }

        if (recipeManager) {
            recipeManager->registerMachineClass(EBF_CONTROLLER, "ebf", 1,
                                                static_cast<uint8_t>(EnergyType::HEAT));
            recipeManager->registerMachineClass(LCR_CONTROLLER, "chemical_reactor", 0,
                                                static_cast<uint8_t>(EnergyType::ELECTRICITY));
        }
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
    auto itemClient         = std::make_shared<simcore::ItemClient>(routerClient);
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

    auto simulationEngine = std::make_shared<simcore::SimulationEngine>();
    if (machineRegistry) {
        simulationEngine->setMachineRegistry(machineRegistry.get());
    }

    simcore::MainThreadQueue mainQueue;

    simulationEngine->onMachineCreated = [eventPublisher, &simulationEngine, entityStateClient, &mainQueue](
        int32_t x, int32_t y, int32_t z, uint16_t machine_id) {
        EnergyType etype = EnergyType::ELECTRICITY;
        if (auto machEntity = findEntityAt(simulationEngine->reg(), x, y, z); machEntity != entt::null) {
            if (auto* es = simulationEngine->reg().try_get<simcore::EnergyStorage>(machEntity)) {
                etype = es->type;
            }
        } else if (auto* reg = MachineRegistry::instance()) {
            if (auto* info = reg->Get(machine_id)) {
                if (info->energy_in.has_value()) etype = info->energy_in.value();
                else if (info->energy_out.has_value()) etype = info->energy_out.value();
            }
        }
        eventPublisher->publishBlockEntityUpdate(x, y, z, machine_id, {}, 0.0f, 0, etype);

        // Try to restore previously saved machine state
        auto entity = findEntityAt(simulationEngine->reg(), x, y, z);
        if (entity != entt::null) {
            entityStateClient->LoadEntityState(0, x, y, z, machine_id,
                [&mainQueue, regPtr = &simulationEngine->reg(), x, y, z](const simcore::EntityStateStoreClient::EntityStateData& state) {
                    mainQueue.push([regPtr, x, y, z, state]() {
                        auto& reg = *regPtr;
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

                        // Don't let stale EntityState reduce slot count below
                        // MachineRegistry value (slot_count). The state may be
                        // from an older code version that saved fewer slots.
                        size_t prevSize = container->slots.size();
                        uint16_t minSlots = std::max<uint16_t>(container->slot_count, 1);
                        container->slots.clear();
                        for (size_t i = 0; i < inv->slots()->size(); ++i) {
                            auto* s = inv->slots()->Get(i);
                            container->slots.emplace_back(s->item_id(),
                                static_cast<uint8_t>(s->count()), s->meta());
                        }
                        // Pad with empty slots if restored state is too short
                        while (container->slots.size() < minSlots) {
                            container->slots.emplace_back(0, 0, 0);
                        }
                        spdlog::debug("[Diagnostic] Restored InventoryContainer at ({},{},{}): "
                                      "prev_slots={} restored_slots={} slot_count={} min={} final={}",
                                      x, y, z, prevSize, inv->slots()->size(),
                                      container->slot_count, minSlots, container->slots.size());
                    });
                });
        }

        spdlog::debug("Published BlockEntityUpdate for new multiblock at ({},{},{}) type={}",
                       x, y, z, machine_id);
    };

    simulationEngine->onMultiblockCreated =
        [eventPublisher, entityStateClient, &simulationEngine, &mainQueue](
            uint64_t controller_id, int32_t x, int32_t y, int32_t z,
            uint16_t mb_type) {
            eventPublisher->publishMultiblockCreated(controller_id, x, y, z, mb_type);
            entityStateClient->LoadEntityState(0, x, y, z, 4,
                [&mainQueue, enginePtr = simulationEngine.get(), controller_id](
                    const simcore::EntityStateStoreClient::EntityStateData& state) {
                    mainQueue.push([enginePtr, controller_id, state]() {
                        enginePtr->deserializeMultiblock(controller_id,
                                                         state.state.data(),
                                                         state.state.size());
                    });
                });
        };

    simulationEngine->onMultiblockDestroyed =
        [eventPublisher](uint64_t controller_id) {
            eventPublisher->publishMultiblockDestroyed(controller_id);
        };

    simulationEngine->onMultiblockSave =
        [entityStateClient](uint64_t controller_id, const std::vector<uint8_t>& state) {
            if (state.empty()) return;
            auto fb = flatbuffers::GetRoot<Protocol::MultiblockState>(state.data());
            entityStateClient->SaveEntityState(0, fb->anchor_x(), fb->anchor_y(),
                                               fb->anchor_z(), 4, state,
                                               [](bool) {});
            spdlog::debug("Saved multiblock #{} state", controller_id);
        };

    // ── ECS Systems ───────────────────────────────────────────────────────

    // Per-player open-container session (Phase B: chest). Persists to
    // EntityStateStore via ChestStateManager (MachineState blob, type 3).
    auto chestSessions = std::make_shared<simcore::ContainerSessionRegistry>();
    auto chestStateManager = std::make_shared<simcore::ChestStateManager>(
        entityStateClient, 0);

    if (machineRegistry) {
        simulationEngine->registerSystem(
            std::make_unique<simcore::AdjacencyTransferSystem>(simulationEngine->reg(), *machineRegistry, eventPublisher));
    }
    simcore::MachineSystem* machineSystemRaw = nullptr;
    {
        auto ms = std::make_unique<simcore::MachineSystem>(
            simulationEngine->reg(), recipeManager, eventPublisher, pipeEnergyClient, itemClient,
            chestSessions, inventoryStore, routerClient);
        machineSystemRaw = ms.get();
        simulationEngine->registerSystem(std::move(ms));
    }
    simcore::BatteryBufferSystem* batteryBufferRaw = nullptr;
    {
        auto bbs = std::make_unique<simcore::BatteryBufferSystem>(
            simulationEngine->reg(), pipeEnergyClient);
        batteryBufferRaw = bbs.get();
        simulationEngine->registerSystem(std::move(bbs));
    }
    spawnECSSystems(blockRepository, eventPublisher, pipeEnergyClient, simulationEngine);

    simulationEngine->registerSystem(std::make_unique<simcore::EBFSystem>(
        simulationEngine->reg(), simulationEngine->getControllers(),
        simulationEngine->getPatternRegistry(),
        recipeManager, eventPublisher, pipeEnergyClient));
    simulationEngine->registerSystem(std::make_unique<simcore::LargeBoilerSystem>(
        simulationEngine->reg(), simulationEngine->getControllers(),
        simulationEngine->getPatternRegistry(),
        eventPublisher, pipeEnergyClient, itemClient));
    simulationEngine->registerSystem(std::make_unique<simcore::LCRSystem>(
        simulationEngine->reg(), simulationEngine->getControllers(),
        simulationEngine->getPatternRegistry(),
        recipeManager, eventPublisher, pipeEnergyClient));

    // ── Generic machine interaction handlers ──
    simulationEngine->registerMachineInteractionHandler(
        simcore::RotareGeneratorSystem::kRotareGeneratorBlockId,
        [engine = simulationEngine.get()](int32_t x, int32_t y, int32_t z, uint64_t) -> bool {
            return engine->tryActivateRotareGenerator(x, y, z);
        });

    auto wrenchHandler = std::make_shared<simcore::WrenchHandler>(
        simulationEngine->reg(), eventPublisher, entityStateClient,
        &simulationEngine->getControllers());

    simcore::WorldContainerInventory worldContainers(
        simulationEngine->reg(), entityStateClient);

    auto wbStateManager = std::make_shared<simulation_core::WorkbenchStateManager>(
        entityStateClient, 0);

    // ── Questbook ─────────────────────────────────────────────────────────
    auto questData = std::make_shared<quest::QuestData>();
    if (!questData->LoadCSV("data/quests/quests.csv")) {
        spdlog::warn("Failed to load quests.csv");
    }
    if (!questData->LoadGraph("data/quests/quest_graph.json")) {
        spdlog::warn("Failed to load quest_graph.json");
    }
    if (!questData->LoadRequirementsJSON("data/quests/quest_requirements.json")) {
        spdlog::warn("Failed to load quest_requirements.json");
    }
    auto questGraph = std::make_shared<quest::QuestGraph>();
    std::unordered_map<uint32_t, std::vector<uint32_t>> prereqsMap;
    for (const auto& qd : questData->AllQuests()) {
        prereqsMap[qd.id] = qd.prerequisites;
    }
    questGraph->Init(questData->Graph(), prereqsMap);
    auto questManager = std::make_shared<simcore::QuestManager>(
        questData.get(), questGraph.get(),
        [routerClient](const std::string& topic, const uint8_t* data, size_t len) {
            routerClient->PublishRaw(topic, data, len);
        });
    spdlog::info("Questbook initialized: {} quests loaded", questData->Count());

    simcore::SimCoreMessageHandler::Deps msgDeps;
    msgDeps.mainQueue = &mainQueue;
    msgDeps.engine = simulationEngine;
    msgDeps.routerClient = routerClient;
    msgDeps.eventPublisher = eventPublisher;
    msgDeps.pipeEnergyClient = pipeEnergyClient;
    msgDeps.fluidClient = fluidClient;
    msgDeps.itemClient = itemClient;
    msgDeps.inventoryStore = inventoryStore;
    msgDeps.entityStateClient = entityStateClient;
    msgDeps.recipeManager = recipeManager;
    msgDeps.blockRepository = blockRepository;
    msgDeps.wrenchHandler = wrenchHandler;
    msgDeps.questManager = questManager;
    msgDeps.machineSystem = machineSystemRaw;
    msgDeps.batteryBuffer = batteryBufferRaw;
    msgDeps.wbStateManager = wbStateManager;
    msgDeps.chestSessions = chestSessions;
    msgDeps.chestStateManager = chestStateManager;
    simcore::SimCoreMessageHandler messageHandler(std::move(msgDeps));
    messageHandler.setup();
    messageHandler.wireOnMessage(worldContainers);

    // ── Router message handler (composition root — wires topics to services) ──
    routerClient->SetServiceName("simcore");
    routerClient->Connect(router_host, router_port);
    chunkstoreClient->Connect(chunkstore_host, chunkstore_port);

    routerClient->Subscribe("player.actions");
    routerClient->Subscribe("player.actions.setblock");
    routerClient->Subscribe("world.blocks.changed");
    routerClient->Subscribe("fluid.consume.response");
    routerClient->Subscribe("item.flow");
    routerClient->Subscribe("item.transfer.response");
    routerClient->Subscribe("player.chest.open");
    routerClient->Subscribe("player.chest.close");
    routerClient->Subscribe("player.machine.open");
    routerClient->Subscribe("player.machine.close");
    routerClient->Subscribe("player.gamemode.change");
    routerClient->Subscribe("player.scenario.start");
    routerClient->Subscribe("player.inventory.load");
    routerClient->Subscribe("meta_db.quest.get.response");
    routerClient->Subscribe("quest.complete.request");

    // Subscribe to ALL topic-handler topics registered in messageHandler.setup()
    // (covers: player.inventory.actions, player.machine.slot, player.tool.action,
    //  player.joined, player.wrench.action, sim.craft.request, recipe.completed,
    //  energy.flow, fluid.flow, item.flow)
    messageHandler.subscribeAll();

    spdlog::info("SimulationCore running, waiting for messages...");

    // ── Main loop ─────────────────────────────────────────────────────────
    std::thread ioThread([&io]() { io.run(); });

    constexpr auto TICK_INTERVAL = std::chrono::milliseconds(50);
    auto nextTick = std::chrono::steady_clock::now();
    auto lastHb   = std::chrono::steady_clock::now();

    while (!g_stop) {
        auto now = std::chrono::steady_clock::now();

        if (metrics.poll()) {
            metrics.printMetrics("SimulationCore Service (simcored)",
                std::string("Tick Interval: ") + std::to_string(TICK_INTERVAL.count()) + " ms");
        }

        if (now - lastHb >= std::chrono::seconds(20)) {
            lastHb = now;
            routerClient->SendHeartbeat();
        }
        if (now >= nextTick) {
            mainQueue.drain();
            simulationEngine->tickAll(1.0f);
            nextTick = now + TICK_INTERVAL;
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
