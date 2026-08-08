#include "Actions/SetBlockCASHandler.h"
#include "Storage/IBlockRepository.h"
#include "Storage/PlayerInventoryStore.h"
#include "Network/IEventPublisher.h"
#include "Network/clients/EntityStateStoreClient.h"
#include "ECS/SimulationEngine.h"
#include "World/BlockTransforms.h"
#include "core_generated.h"
#include "machine_state_generated.h"
#include <common/ItemId.h>
#include <data/registry/ToolIds.h>
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <array>

namespace simcore {

#define TRACE_LOG(tid, svc, op, dur_us) \
    spdlog::info("[TRACE tid={}] {} {} {}us", (tid), (svc), (op), (dur_us))

// Face index (0=DOWN..5=EAST) → adjacent cell offset, used for placement
// when the client right-clicks an existing block.
static void faceAdjacent(uint8_t face, int32_t& x, int32_t& y, int32_t& z) {
    switch (face) {
        case 0: --y; break; // DOWN
        case 1: ++y; break; // UP
        case 2: --z; break; // NORTH
        case 3: ++z; break; // SOUTH
        case 4: --x; break; // WEST
        case 5: ++x; break; // EAST
        default: break;
    }
}

// Held item is a mining tool → the machine should be broken, not interacted.
static bool isMiningTool(uint16_t item) {
    return item == ITEM_DRILL_ULV || item == ITEM_DRILL_LV ||
           item == ITEM_DRILL_MV || item == ITEM_DRILL_HV ||
           item == ITEM_CHAINSAW_LV;
}

// Dry-run: add every multiblock content stack into `inv` (a copy of the
// player's inventory). Mirrors PlayerInventoryStore::giveItem stacking
// (by item_id, max 64). Returns false and leaves `inv` in a partial state if
// anything does not fit — the caller must then NOT apply the change.
static bool canFitAll(std::array<PersistSlot, kInventorySlots>& inv,
                      const std::vector<InventorySlot>& items) {
    constexpr uint8_t kMaxStack = 64;
    for (const auto& item : items) {
        if (item.item_id == 0) continue;
        int remaining = static_cast<int>(item.count);

        // Stack onto existing matching stacks first
        for (auto& s : inv) {
            if (remaining <= 0) break;
            if (s.item_id == item.item_id && s.count < kMaxStack) {
                uint8_t room = kMaxStack - s.count;
                uint8_t add = std::min(static_cast<uint8_t>(remaining), room);
                s.count = static_cast<uint8_t>(s.count + add);
                remaining -= add;
            }
        }
        // Then fill empty slots
        for (auto& s : inv) {
            if (remaining <= 0) break;
            if (s.item_id == 0) {
                uint8_t add = std::min(static_cast<uint8_t>(remaining), kMaxStack);
                s = {item.item_id, add, item.meta};
                remaining -= add;
            }
        }
        if (remaining > 0) return false;
    }
    return true;
}

SetBlockCASHandler::SetBlockCASHandler(std::shared_ptr<IBlockRepository> repo,
                                       std::shared_ptr<IEventPublisher> publisher,
                                       std::shared_ptr<SimulationEngine> engine,
                                       std::shared_ptr<PlayerInventoryStore> inventoryStore,
                                       ItemGiveCallback onGiveItem,
                                       DrillUseCallback onDrillUse,
                                       BlockPlacedCallback onBlockPlaced,
                                       PostCallback postToMain)
    : repo_(std::move(repo))
    , publisher_(std::move(publisher))
    , engine_(std::move(engine))
    , inventoryStore_(std::move(inventoryStore))
    , onGiveItem_(std::move(onGiveItem))
    , onDrillUse_(std::move(onDrillUse))
    , onBlockPlaced_(std::move(onBlockPlaced))
    , postToMain_(std::move(postToMain))
{}

entt::entity SetBlockCASHandler::findEntityAt(int32_t x, int32_t y, int32_t z) const {
    auto& reg = engine_->reg();
    auto vw = reg.view<const simcore::Position>();
    for (auto e : vw) {
        auto& pp = vw.get<const simcore::Position>(e);
        if (static_cast<int32_t>(pp.x) == x &&
            static_cast<int32_t>(pp.y) == y &&
            static_cast<int32_t>(pp.z) == z) return e;
    }
    return entt::null;
}

void SetBlockCASHandler::publishMachineState(int32_t x, int32_t y, int32_t z,
                                             uint16_t machine_id, uint64_t player_id,
                                             uint32_t request_id) {
    engine_->onMachineInteracted(x, y, z, machine_id, player_id);

    // Report the entity's real state so the client's MachineWindow opens with
    // the correct energy type/level (not a hardcoded 0/EU).
    auto* machineReg = engine_->getMachineRegistry();
    EnergyType etype = EnergyType::ELECTRICITY;
    uint32_t energy = 0;
    uint32_t capacity = 0;
    int slotsIn = -1;

    if (auto ent = findEntityAt(x, y, z); ent != entt::null) {
        if (auto* es = engine_->reg().try_get<simcore::EnergyStorage>(ent)) {
            etype = es->type;
            energy = static_cast<uint32_t>(es->current);
            capacity = static_cast<uint32_t>(es->capacity);
        }
        if (auto* mc = engine_->reg().try_get<simcore::MachineComponent>(ent)) {
            if (machineReg) {
                if (auto* info = machineReg->Get(mc->machine_id)) {
                    slotsIn = info->slots_in;
                }
            }
        }
    } else if (machineReg) {
        if (auto* info = machineReg->Get(machine_id)) {
            if (info->energy_in.has_value()) etype = info->energy_in.value();
            else if (info->energy_out.has_value()) etype = info->energy_out.value();
        }
    }

    publisher_->publishBlockEntityUpdate(x, y, z, machine_id, {}, 0.0f, energy, etype, capacity, slotsIn);
    publisher_->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_ACCEPTED),
                                x, y, z, machine_id, 0, "Machine interacted", request_id);
    publisher_->publishBlockDirective(static_cast<uint8_t>(Protocol::BlockDirective_OPEN_UI),
                                      machine_id, x, y, z, request_id);
}

void SetBlockCASHandler::publishChestState(int32_t x, int32_t y, int32_t z,
                                           uint16_t chest_id, uint64_t player_id,
                                           uint32_t request_id) {
    // Load chest inventory from EntityStateStore and publish in BlockEntityUpdate
    if (entityStateClient_) {
        entityStateClient_->LoadEntityState(0, x, y, z, kChestEntityType,
            [this, x, y, z, chest_id, player_id, request_id](
                const EntityStateStoreClient::EntityStateData& state) {
                std::vector<uint8_t> inventory_data;
                if (!state.state.empty()) {
                    auto verifier = flatbuffers::Verifier(state.state.data(), state.state.size());
                    if (verifier.VerifyBuffer<Protocol::MachineState>(nullptr)) {
                        auto fbState = flatbuffers::GetRoot<Protocol::MachineState>(state.state.data());
                        auto* inv = fbState->inventory();
                        if (inv && inv->slots()) {
                            inventory_data.resize(inv->slots()->size() * 5);
                            uint8_t* ptr = inventory_data.data();
                            for (size_t i = 0; i < inv->slots()->size(); ++i) {
                                auto* s = inv->slots()->Get(i);
                                uint16_t id = s ? s->item_id() : 0;
                                uint8_t cnt = s ? static_cast<uint8_t>(s->count()) : 0;
                                uint16_t mt = s ? s->meta() : 0;
                                std::memcpy(ptr, &id, sizeof(uint16_t)); ptr += sizeof(uint16_t);
                                *ptr++ = cnt;
                                std::memcpy(ptr, &mt, sizeof(uint16_t)); ptr += sizeof(uint16_t);
                            }
                        }
                    }
                }
                if (inventory_data.empty()) inventory_data.resize(27 * 5, 0);
                publisher_->publishBlockEntityUpdate(x, y, z, chest_id, inventory_data, 0.0f, 0,
                                                     EnergyType::ELECTRICITY, 0, 27);
            });
    } else {
        // No entity store — publish empty inventory
        std::vector<uint8_t> empty(27 * 5, 0);
        publisher_->publishBlockEntityUpdate(x, y, z, chest_id, empty, 0.0f, 0);
    }
}

void SetBlockCASHandler::handleMachineInteraction(int32_t x, int32_t y, int32_t z,
                                                  uint16_t machine_id, uint64_t player_id,
                                                  uint32_t request_id) {
    // A machine the player right-clicks may predate this simcore instance
    // (persisted in ChunkStore before a restart). ECS machine entities are
    // created ONLY on block-change events (onBlockChanged), so such machines
    // have no entity — and an entity-less machine is invisible to
    // GeneratorSystem, MachineSystem, and HeatTransferSystem (heat can never
    // reach a furnace that has no entity). Lazily create it from ChunkStore,
    // mirroring MachineSlotHandler.
    if (findEntityAt(x, y, z) != entt::null) {
        publishMachineState(x, y, z, machine_id, player_id, request_id);
        return;
    }

    spdlog::warn("SetBlockCASHandler: no ECS entity for machine {} at ({},{},{}) — lazy-init from ChunkStore",
                 machine_id, x, y, z);
    repo_->getBlock(x, y, z,
        [this, x, y, z, machine_id, player_id, request_id](const BlockData& bd) {
            uint16_t finalId = machine_id;
            if (bd.block_id != 0) {
                finalId = bd.block_id;
                engine_->onBlockChanged(static_cast<uint32_t>(x),
                                        static_cast<uint32_t>(y),
                                        static_cast<uint32_t>(z),
                                        bd.block_id, bd.meta, bd.mb_id);
                spdlog::info("[SimCore] Lazy-created ECS entity at ({},{},{}) block_id={}",
                             x, y, z, bd.block_id);
            }
            // The actual block may no longer be what the client expected —
            // only treat this as a machine interaction if it really is one.
            auto* machineReg = engine_->getMachineRegistry();
            if (!machineReg || !machineReg->IsMachine(finalId)) {
                spdlog::warn("SetBlockCASHandler: block {} at ({},{},{}) is not a machine — reject",
                             finalId, x, y, z);
                publisher_->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_REJECTED),
                                            x, y, z, finalId, 0,
                                            "Block is not a machine", request_id);
                return;
            }
            publishMachineState(x, y, z, finalId, player_id, request_id);
        });
}

void SetBlockCASHandler::handle(const void *table) {
    handle(static_cast<const Protocol::SetBlockAction*>(table));
}

void SetBlockCASHandler::handle(const Protocol::SetBlockAction *action)
{
    auto action_type = action->action();

    int32_t x = action->pos()->x();
    int32_t y = action->pos()->y();
    int32_t z = action->pos()->z();
    uint16_t expected_block_id = action->expected_block_id();
    uint16_t new_block_id = action->new_block_id();
    uint64_t player_id = action->player_id();
    uint32_t request_id = action->request_id();

    if (action_type == Protocol::PlayerActionType_RIGHT_MOUSE_CLICK/* && new_block_id == 0*/) {
        if (engine_) {
            auto* machineReg = engine_->getMachineRegistry();
            if (machineReg && machineReg->IsMachine(expected_block_id)) {
                handleMachineInteraction(x, y, z, expected_block_id, player_id, request_id);
                return;
            }
        }
        // Chest (packed ID 0:10:11:0 → ItemId::pack("0:10:11:0")).
        if (expected_block_id == ItemId::pack("0:10:11:0")) {
            spdlog::info("SetBlockCASHandler: chest interact at ({},{},{})", x, y, z);
            publisher_->publishBlockAck(
                static_cast<uint8_t>(Protocol::BlockAckStatus_ACCEPTED),
                x, y, z, expected_block_id, 0, nullptr, request_id,
                static_cast<uint8_t>(action_type));
            publisher_->publishBlockDirective(
                static_cast<uint8_t>(Protocol::BlockDirective_OPEN_UI),
                expected_block_id, x, y, z, request_id,
                static_cast<uint8_t>(action_type));
            publishChestState(x, y, z, expected_block_id, player_id, request_id);
            return;
        }
        if (action->held_item() == 0 || isMiningTool(action->held_item()) ||
            action->held_item() == ITEM_WRENCH) {
            spdlog::warn("SetBlockCASHandler: nothing placeable in hand at ({},{},{})",
                         x, y, z);
            publisher_->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_REJECTED),
                                        x, y, z, expected_block_id, 0,
                                        "Nothing placeable in hand",
                                        request_id,
                                        static_cast<uint8_t>(action_type));
            return;
        }
    }

    // ── Left-click machine interaction (e.g. rotare_generator: click to spin)
    // Only machines flagged interact_on_left; mining tools in hand still break.
    if (action_type == Protocol::PlayerActionType_LEFT_MOUSE_CLICK && engine_) {
        auto* machineReg = engine_->getMachineRegistry();
        auto* info = machineReg ? machineReg->Get(expected_block_id) : nullptr;
        if (info && info->interact_on_left && !isMiningTool(action->held_item())) {
            spdlog::info("SetBlockCASHandler: left-click spin machine {} at ({},{},{})",
                         expected_block_id, x, y, z);
            engine_->onMachineInteracted(x, y, z, expected_block_id, player_id);
            publisher_->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_ACCEPTED),
                                        x, y, z, expected_block_id, 0,
                                        "Machine spun", request_id,
                                        static_cast<uint8_t>(action_type));
            publisher_->publishBlockDirective(
                static_cast<uint8_t>(Protocol::BlockDirective_PLAY_ANIMATION),
                1 /* spin effect */, x, y, z, request_id,
                static_cast<uint8_t>(action_type));
            return;
        }
    }

    // Right-click on an existing block places on the face-adjacent cell.
    int32_t eff_x = x, eff_y = y, eff_z = z;
    uint16_t eff_expected = expected_block_id;
    if (action_type == Protocol::PlayerActionType_RIGHT_MOUSE_CLICK) {
        faceAdjacent(action->face(), eff_x, eff_y, eff_z);
        eff_expected = 0; // place against air on the adjacent cell
    }

    uint16_t final_block_id =
        (action_type == Protocol::PlayerActionType_LEFT_MOUSE_CLICK)
            ? 0
            : (action_type == Protocol::PlayerActionType_RIGHT_MOUSE_CLICK)
                  ? action->held_item()
                  : new_block_id;
    uint8_t final_meta = 0;

    // ── Multiblock block-break guard (task 2.1) ─────────────────────────────
    // Breaking ANY block of a multiblock returns ALL hatch+controller contents
    // to the breaking player. If they do not fit, refuse the break — the block
    // SHALL NOT break and no items are dropped.
    if (action_type == Protocol::PlayerActionType_LEFT_MOUSE_CLICK && engine_ && inventoryStore_) {
        auto owner = engine_->findControllerAt(static_cast<uint32_t>(x),
                                               static_cast<uint32_t>(y),
                                               static_cast<uint32_t>(z));
        if (owner != engine_->getControllers().end()) {
            std::vector<simcore::InventorySlot> contents;
            engine_->collectControllerContents(owner->second, contents);
            auto inv = inventoryStore_->getSlots(player_id);
            if (!contents.empty() && !canFitAll(inv, contents)) {
                spdlog::warn("Refusing to break multiblock at ({},{},{}): contents do not fit player {} inventory",
                             x, y, z, player_id);
                publisher_->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_REJECTED),
                                            x, y, z, expected_block_id, 0,
                                            "Multiblock contents do not fit in inventory",
                                            request_id,
                                            static_cast<uint8_t>(action_type));
                return;
            }
            if (!contents.empty()) {
                inventoryStore_->setSlots(player_id, inv);
                spdlog::info("Multiblock contents returned to player {} on break at ({},{},{})",
                             player_id, x, y, z);
            }
        }
    }

    auto transform = applyBlockTransform(eff_expected, final_block_id, final_meta);
    if (transform.has_value()) {
        final_block_id = transform->new_block_id;
        final_meta = transform->new_meta;
        spdlog::info("Block transformation applied: new_id={}", final_block_id);
    }

    publisher_->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_ACCEPTED),
        eff_x, eff_y, eff_z, final_block_id, final_meta, nullptr, request_id,
        static_cast<uint8_t>(action_type));

    auto cas_t0 = std::chrono::steady_clock::now();
    repo_->setBlockCAS(eff_x, eff_y, eff_z, eff_expected, final_block_id, final_meta,
        [this, eff_x, eff_y, eff_z, eff_expected, final_block_id, final_meta, held_item = action->held_item(), player_id, action_type, request_id, cas_t0](const CASResult& result) {
            auto cas_dur = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - cas_t0).count();
            TRACE_LOG(request_id, "cas_cb", "complete", cas_dur);
            auto processResult = [this, eff_x, eff_y, eff_z, eff_expected, final_block_id, final_meta, held_item, player_id, action_type, request_id](const CASResult& result) {
                if (result.status == 0) {
                    spdlog::info("Block CAS OK at ({},{},{}) final_id={}", eff_x, eff_y, eff_z, final_block_id);

                    // Break: give the broken block to the player
                    if (action_type == Protocol::PlayerActionType_LEFT_MOUSE_CLICK) {
                        uint16_t broken_block = eff_expected;
                        if (broken_block != 0 && onGiveItem_) {
                            spdlog::info("Giving block {} to player {}", broken_block, player_id);
                            onGiveItem_(player_id, broken_block, 1, -1);
                        }
                        if (onDrillUse_) {
                            onDrillUse_(player_id, eff_x, eff_y, eff_z, broken_block);
                        }
                    }

                    // Place: consume the placed block from the player's inventory
                    if (action_type == Protocol::PlayerActionType_RIGHT_MOUSE_CLICK) {
                        uint16_t placed_block = held_item;
                        if (placed_block != 0 && inventoryStore_) {
                            auto slots = inventoryStore_->getSlots(player_id);
                            for (auto& s : slots) {
                                if (s.item_id == placed_block && s.count > 0) {
                                    s.count--;
                                    if (s.count == 0) s = {};
                                    spdlog::info("Placed block {} by player {} — consumed from inv",
                                                 placed_block, player_id);
                                    break;
                                }
                            }
                            inventoryStore_->setSlots(player_id, slots);
                        }
                    }

                    publisher_->publishBlockChangedEvent(eff_x, eff_y, eff_z, final_block_id, final_meta, request_id, player_id);
                    if (engine_) {
                        engine_->onBlockChanged(static_cast<uint32_t>(eff_x),
                                                static_cast<uint32_t>(eff_y),
                                                static_cast<uint32_t>(eff_z),
                                                final_block_id, final_meta, 0);
                    }
                    if (action_type == Protocol::PlayerActionType_RIGHT_MOUSE_CLICK && onBlockPlaced_) {
                        onBlockPlaced_(player_id, eff_x, eff_y, eff_z, final_block_id);
                    }
                } else { // CONFLICT
                    spdlog::warn("Block CAS CONFLICT at ({},{},{}) actual_id={}, from_id={}, to_id={}", eff_x, eff_y, eff_z, result.block_id, eff_expected, final_block_id);
                    publisher_->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_CONFLICT),
                                                eff_x, eff_y, eff_z, result.block_id, result.meta, nullptr, request_id,
                                                static_cast<uint8_t>(action_type));
                }
            };
            if (postToMain_) {
                postToMain_([processResult, result]() { processResult(result); });
            } else {
                processResult(result);
            }
        });
}

} // namespace simcore