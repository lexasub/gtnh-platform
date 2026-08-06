#include "Network/SimCoreMessageHandler.h"
#include "Common/MainThreadQueue.h"
#include "Network/RouterEventPublisher.h"
#include "Network/PipeEnergyClient.h"
#include "Network/FluidClient.h"
#include "Network/ItemClient.h"
#include "Network/TopicDispatcher.h"
#include "Network/clients/EntityStateStoreClient.h"
#include "Network/clients/IoUringRouterClient.h"
#include "Network/clients/IoUringChunkClient.h"
#include "ECS/SimulationEngine.h"
#include "ECS/Systems/MachineSystem.h"
#include "ECS/Systems/BatteryBufferSystem.h"
#include "Actions/SetBlockCASHandler.h"
#include "Actions/ActionDispatcher.h"
#include "Actions/MiningCalculator.h"
#include "Actions/WrenchHandler.h"
#include "Actions/WrenchActionHandler.h"
#include "World/ChunkEventHandler.h"
#include "World/WorldContainerInventory.h"
#include "Storage/ChunkStoreRepository.h"
#include "Storage/PlayerInventoryStore.h"
#include "Scenario/GameScenario.h"
#include "Crafting/CraftRequestHandler.h"
#include "Crafting/RecipeCompletedHandler.h"
#include "Quest/QuestManager.h"
#include "Actions/MachineSlotHandler.h"
#include "Actions/ToolActionHandler.h"
#include "Storage/InventoryLoadHandler.h"
#include "Storage/InventoryActionHandler.h"
#include "Storage/PlayerJoinedHandler.h"
#include "ECS/Reactors/EnergyFlowHandler.h"
#include "ECS/Reactors/FluidFlowHandler.h"
#include "ECS/Reactors/ItemFlowHandler.h"
#include "ECS/Reactors/CableExplosionHandler.h"
#include "../../data/registry/ToolIds.h"
#include "core_generated.h"
#include "quest_generated.h"
#include "machine_state_generated.h"
#include "pipe_network_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>
#include <cstring>

namespace simcore {

SimCoreMessageHandler::SimCoreMessageHandler(Deps deps)
    : deps_(std::move(deps))
{}

void SimCoreMessageHandler::setup() {
    auto& d = deps_;

    topicDispatcher_ = std::make_shared<TopicDispatcher>();

    topicDispatcher_->on("energy.flow", std::make_unique<EnergyFlowHandler>(
        d.engine->reg(), d.pipeEnergyClient));
    topicDispatcher_->on("fluid.flow", std::make_unique<FluidFlowHandler>(
        d.engine->reg(), d.fluidClient));
    topicDispatcher_->on("item.flow", std::make_unique<ItemFlowHandler>(
        d.engine->reg(), d.itemClient, d.routerClient, d.entityStateClient));

    topicDispatcher_->on("energy.cable.exploded", std::make_unique<CableExplosionHandler>(
        d.chunkClient));

    topicDispatcher_->on("sim.craft.request", std::make_unique<CraftRequestHandler>(
        d.routerClient, d.recipeManager, d.inventoryStore, d.questManager));
    topicDispatcher_->on("recipe.completed", std::make_unique<RecipeCompletedHandler>(
        d.engine));

    topicDispatcher_->on("player.machine.slot", std::make_unique<MachineSlotHandler>(
        d.engine, d.inventoryStore, d.entityStateClient, d.eventPublisher, d.routerClient));
    topicDispatcher_->on("player.tool.action", std::make_unique<ToolActionHandler>(
        d.engine, d.inventoryStore, d.routerClient, d.questManager));

    topicDispatcher_->on("player.inventory.load", std::make_unique<InventoryLoadHandler>(
        d.inventoryStore, d.routerClient));
    topicDispatcher_->on("player.inventory.actions", std::make_unique<InventoryActionHandler>(
        d.inventoryStore, d.routerClient));
    topicDispatcher_->on("player.joined", std::make_unique<PlayerJoinedHandler>(
        d.inventoryStore, d.routerClient, d.questManager));

    auto postToMainThread = [&d](std::function<void()> fn) {
        d.mainQueue->push(std::move(fn));
    };

    casHandler_ = std::make_shared<SetBlockCASHandler>(
        d.blockRepository, d.eventPublisher, d.engine,
        d.inventoryStore,
        [inventoryStore = d.inventoryStore](uint64_t player_id, uint16_t item_id, uint8_t count, int32_t target_slot) {
            inventoryStore->giveItem(player_id, item_id, count, target_slot);
        },
        [inventoryStore = d.inventoryStore](uint64_t player_id, int32_t x, int32_t y, int32_t z, uint16_t block_id) {
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
        },
        [questManager = d.questManager](uint64_t player_id, int32_t x, int32_t y, int32_t z, uint16_t block_id) {
            if (questManager) {
                questManager->checkBlockAction(player_id, x, y, z, block_id);
            }
        },
        postToMainThread);

    dispatcher_ = std::make_shared<ActionDispatcher>(
        [inventoryStore = d.inventoryStore](uint64_t player_id, uint16_t item_id, uint8_t count, int32_t target_slot) {
            inventoryStore->giveItem(player_id, item_id, count, target_slot);
        });

    chunkHandler_ = std::make_shared<ChunkEventHandler>(d.engine);

    auto wrenchActionHandler = std::make_unique<WrenchActionHandler>(d.wrenchHandler, d.questManager);
    wrenchActionHandler->setRouter(d.routerClient);
    topicDispatcher_->on("player.wrench.action", std::move(wrenchActionHandler));
}

void SimCoreMessageHandler::wireOnMessage(WorldContainerInventory& worldContainers) {
    auto& d = deps_;
    auto& mainQueue = *d.mainQueue;
    auto& dispatcher = *dispatcher_;
    auto& casHandler = *casHandler_;
    auto& chunkHandler = *chunkHandler_;
    auto* batteryBuffer = d.batteryBuffer;
    auto* machineSystem = d.machineSystem;
    auto entityStateClient = d.entityStateClient;
    auto routerClient = d.routerClient;
    auto topicDispatcher = topicDispatcher_;
    auto questManager = d.questManager;
    auto inventoryStore = d.inventoryStore;

    routerClient->OnMessage([&mainQueue, &dispatcher, &casHandler, &chunkHandler, &worldContainers,
                             topicDispatcher, routerClient, entityStateClient, inventoryStore,
                             batteryBuffer, machineSystem, questManager]
                            (const std::string& topic, const std::vector<uint8_t>& data) {
        // Filter player.actions on the io thread, BEFORE mainQueue: the client
        // floods UNLOAD/MOVE/CHUNK_REQUEST at ~15k/s while walking (chunk
        // eviction).  simcore only handles ITEM_ACTION on this topic — queueing
        // the rest starves player.actions.setblock by seconds.
        if (topic == "player.actions") {
            flatbuffers::Verifier v(data.data(), data.size());
            if (v.VerifyBuffer<Protocol::PlayerAction>(nullptr)) {
                auto* pa = flatbuffers::GetRoot<Protocol::PlayerAction>(data.data());
                if (!pa || pa->action() != Protocol::PlayerActionType_ITEM_ACTION) {
                    return;
                }
            } else {
                return;
            }
        }
        mainQueue.push([&, topic, data]() {
            if (topic == "player.actions.setblock") {
                flatbuffers::Verifier v(data.data(), data.size());
                if (!v.VerifyBuffer<Protocol::SetBlockAction>()) return;
                auto* action = flatbuffers::GetRoot<Protocol::SetBlockAction>(data.data());
                casHandler.handle((void*)action);
            } else if (topic == "player.actions") {
                dispatcher.dispatch(data);
            } else if (topic == "world.blocks.changed") {
                chunkHandler.handle(data);
            } else if (topic == "energy.consume.response") {
                auto* resp = flatbuffers::GetRoot<Protocol::EnergyConsumeResp>(data.data());
                if (!resp) return;
                auto consumed = resp->consumed();
                auto remaining = resp->remaining();
                if (!batteryBuffer || !batteryBuffer->onConsumeResponse(0, consumed, remaining)) {
                    machineSystem->onConsumeResponse(consumed, remaining);
                }

            } else if (topic == "fluid.consume.response") {
                auto* resp = flatbuffers::GetRoot<Protocol::FluidConsumeResp>(data.data());
                if (!resp) return;
                spdlog::trace("FluidConsumeResp: consumed={} remaining={}",
                               resp->consumed(), resp->remaining());

            } else if (topic == "item.transfer.response") {
                auto* resp = flatbuffers::GetRoot<Protocol::ItemTransferResp>(data.data());
                if (!resp) return;
                spdlog::debug("ItemTransferResp: transferred={} remaining={}",
                              resp->transferred(), resp->remaining());

            } else if (topic == "player.chest.open") {
                flatbuffers::Verifier v(data.data(), data.size());
                if (!v.VerifyBuffer<Protocol::ChestOpenReq>(nullptr)) return;
                auto* req = flatbuffers::GetRoot<Protocol::ChestOpenReq>(data.data());
                if (!req || !req->pos()) return;
                auto* p = req->pos();
                int32_t x = p->x(), y = p->y(), z = p->z();

                if (req->open()) {
                    entityStateClient->LoadEntityState(0, x, y, z, 3,
                        [&mainQueue, routerClient, x, y, z](const EntityStateStoreClient::EntityStateData& state) {
                            mainQueue.push([routerClient, x, y, z, state]() {
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

            } else if (topic == "meta_db.quest.get.response") {
                if (questManager && data.size() >= 4) {
                    uint64_t playerId = 0;
                    flatbuffers::Verifier v(data.data(), data.size());
                    if (v.VerifyBuffer<Protocol::QuestProgressUpdate>(nullptr)) {
                        auto resp = flatbuffers::GetRoot<Protocol::QuestProgressUpdate>(data.data());
                        if (resp) {
                            playerId = resp->player_id();
                        }
                    }
                    questManager->loadProgress(playerId, data);
                }

            } else if (topic == "quest.complete.request") {
                // Client "Complete" button → server-authoritative completion.
                // QuestManager validates status + prerequisites; on acceptance
                // it publishes quest.completed / quest.progress.updated /
                // quest.unlocked so MetaDB grants the reward and the client is
                // notified.
                flatbuffers::Verifier v(data.data(), data.size());
                if (!v.VerifyBuffer<Protocol::QuestCompleteRequest>(nullptr)) {
                    spdlog::warn("[Quest] invalid QuestCompleteRequest");
                    return;
                }
                auto* req = flatbuffers::GetRoot<Protocol::QuestCompleteRequest>(data.data());
                if (req && questManager) {
                    questManager->completeQuest(req->player_id(), req->quest_id());
                }

            } else if (topic == "quest.book.open") {
                // Player opened the quest book → check INVENTORY-type quest
                // objectives against the authoritative player inventory.
                flatbuffers::Verifier v(data.data(), data.size());
                if (!v.VerifyBuffer<Protocol::QuestBookOpen>(nullptr)) {
                    spdlog::warn("[Quest] invalid QuestBookOpen");
                    return;
                }
                auto* req = flatbuffers::GetRoot<Protocol::QuestBookOpen>(data.data());
                if (!req || !questManager || !inventoryStore) return;
                uint64_t pid = req->player_id();
                auto slotsArr = inventoryStore->getSlots(pid);
                std::vector<simcore::PersistSlot> slots(slotsArr.begin(), slotsArr.end());
                questManager->checkInventory(pid, slots);

            } else if (topic == "player.gamemode.change") {
                flatbuffers::Verifier v(data.data(), data.size());
                if (!v.VerifyBuffer<Protocol::GameModeChange>(nullptr)) {
                    spdlog::warn("[SimCore] Invalid GameModeChange payload");
                    return;
                }
                auto* gmc = flatbuffers::GetRoot<Protocol::GameModeChange>(data.data());
                uint64_t pid = gmc->player_id();
                uint8_t mode = static_cast<uint8_t>(gmc->new_mode());
                spdlog::info("[SimCore] GameMode change: player={} mode={}", pid, mode);
                // Store locally
                if (inventoryStore) {
                    inventoryStore->setGameMode(pid, mode);
                }
                // Echo back
                flatbuffers::FlatBufferBuilder fbb(32);
                auto echo = Protocol::CreateGameModeChange(fbb, pid, static_cast<Protocol::GameMode>(mode));
                fbb.Finish(echo);
                std::vector<uint8_t> buf(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
                routerClient->Publish("player.gamemode.changed", std::move(buf));
            } else if (topic == "player.scenario.start") {
                flatbuffers::Verifier v(data.data(), data.size());
                if (!v.VerifyBuffer<Protocol::StartScenarioReq>(nullptr)) {
                    spdlog::warn("[SimCore] Invalid StartScenarioReq payload");
                    return;
                }
                auto* req = flatbuffers::GetRoot<Protocol::StartScenarioReq>(data.data());
                uint64_t pid = req->player_id();
                uint8_t idx = static_cast<uint8_t>(req->scenario_index());
                if (pid == 0) {
                    spdlog::warn("[SimCore] StartScenarioReq with player_id == 0 rejected");
                    return;
                }
                const auto* sc = findScenario(idx);
                if (!sc) {
                    spdlog::warn("[SimCore] StartScenarioReq scenario_index={} out of range", idx);
                    return;
                }
                if (!inventoryStore) return;
                spdlog::info("[SimCore] Start scenario {} for player {}", idx, pid);
                // setSlots/giveItem fire postMutation synchronously, so the
                // authoritative player.inventory.update snapshot is enqueued
                // before the response below reaches the client.
                applyScenario(*inventoryStore, *sc, pid);
                flatbuffers::FlatBufferBuilder fbb(64);
                auto resp = Protocol::CreateStartScenarioResp(
                    fbb, pid, idx, true, 0,
                    static_cast<Protocol::GameMode>(sc->targetMode),
                    sc->questBookEra);
                fbb.Finish(resp);
                std::vector<uint8_t> buf(fbb.GetBufferPointer(),
                                         fbb.GetBufferPointer() + fbb.GetSize());
                routerClient->Publish("player.scenario.start.response", std::move(buf));
            } else if (topicDispatcher->dispatch(topic, data)) {
            } else {
                spdlog::debug("Unhandled topic: {}", topic);
            }
        });
    });
}

void SimCoreMessageHandler::subscribeAll() {
    if (topicDispatcher_) {
        topicDispatcher_->subscribeAll(deps_.routerClient);
    }
}

} // namespace simcore
