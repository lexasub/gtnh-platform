#include "Network/SimCoreMessageHandler.h"
#include "Common/MainThreadQueue.h"
#include "Network/RouterEventPublisher.h"
#include "Network/PipeEnergyClient.h"
#include "Network/FluidClient.h"
#include "Network/ItemClient.h"
#include "Network/TopicDispatcher.h"
#include "Network/clients/EntityStateStoreClient.h"
#include "Network/clients/IoUringRouterClient.h"
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
#include "Crafting/CraftRequestHandler.h"
#include "Crafting/RecipeCompletedHandler.h"
#include "Actions/MachineSlotHandler.h"
#include "Actions/ToolActionHandler.h"
#include "Storage/InventoryLoadHandler.h"
#include "Storage/InventoryActionHandler.h"
#include "Storage/PlayerJoinedHandler.h"
#include "ECS/Reactors/EnergyFlowHandler.h"
#include "ECS/Reactors/FluidFlowHandler.h"
#include "ECS/Reactors/ItemFlowHandler.h"
#include "../../data/registry/ToolIds.h"
#include "core_generated.h"
#include "machine_state_generated.h"
#include "pipe_network_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

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
        d.engine->reg(), d.itemClient));

    topicDispatcher_->on("sim.craft.request", std::make_unique<CraftRequestHandler>(
        d.routerClient, d.recipeManager, d.inventoryStore));
    topicDispatcher_->on("recipe.completed", std::make_unique<RecipeCompletedHandler>(
        d.engine));

    topicDispatcher_->on("player.machine.slot", std::make_unique<MachineSlotHandler>(
        d.engine, d.inventoryStore, d.entityStateClient, d.eventPublisher, d.routerClient));
    topicDispatcher_->on("player.tool.action", std::make_unique<ToolActionHandler>(
        d.engine, d.inventoryStore, d.routerClient));

    topicDispatcher_->on("player.inventory.load", std::make_unique<InventoryLoadHandler>(
        d.inventoryStore, d.routerClient));
    topicDispatcher_->on("player.inventory.actions", std::make_unique<InventoryActionHandler>(
        d.inventoryStore, d.routerClient));
    topicDispatcher_->on("player.joined", std::make_unique<PlayerJoinedHandler>(
        d.inventoryStore));

    auto postToMainThread = [&d](std::function<void()> fn) {
        d.mainQueue->push(std::move(fn));
    };

    casHandler_ = std::make_shared<SetBlockCASHandler>(
        d.blockRepository, d.eventPublisher, d.engine,
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
        postToMainThread);

    dispatcher_ = std::make_shared<ActionDispatcher>(casHandler_,
        [inventoryStore = d.inventoryStore](uint64_t player_id, uint16_t item_id, uint8_t count, int32_t target_slot) {
            inventoryStore->giveItem(player_id, item_id, count, target_slot);
        });

    chunkHandler_ = std::make_shared<ChunkEventHandler>(d.engine);

    auto wrenchActionHandler = std::make_unique<WrenchActionHandler>(d.wrenchHandler);
    wrenchActionHandler->setRouter(d.routerClient);
    topicDispatcher_->on("player.wrench.action", std::move(wrenchActionHandler));
}

void SimCoreMessageHandler::wireOnMessage(WorldContainerInventory& worldContainers) {
    auto& d = deps_;
    auto& mainQueue = *d.mainQueue;
    auto& dispatcher = *dispatcher_;
    auto& chunkHandler = *chunkHandler_;
    auto* batteryBuffer = d.batteryBuffer;
    auto* machineSystem = d.machineSystem;
    auto entityStateClient = d.entityStateClient;
    auto routerClient = d.routerClient;
    auto topicDispatcher = topicDispatcher_;

    routerClient->OnMessage([&mainQueue, &dispatcher, &chunkHandler, &worldContainers,
                             topicDispatcher, routerClient, entityStateClient,
                             batteryBuffer, machineSystem]
                            (const std::string& topic, const std::vector<uint8_t>& data) {
        mainQueue.push([&, topic, data]() {
            if (topic == "player.action" || topic == "player.setblock") {
                dispatcher.dispatch(data, topic);
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

            } else if (topicDispatcher->dispatch(topic, data)) {
            } else {
                spdlog::debug("Unhandled topic: {}", topic);
            }
        });
    });
}

} // namespace simcore
