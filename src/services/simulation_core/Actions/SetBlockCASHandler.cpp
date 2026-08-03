#include "Actions/SetBlockCASHandler.h"
#include "Storage/IBlockRepository.h"
#include "Storage/PlayerInventoryStore.h"
#include "Network/IEventPublisher.h"
#include "ECS/SimulationEngine.h"
#include "World/BlockTransforms.h"
#include "core_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <array>

namespace simcore {

#define TRACE_LOG(tid, svc, op, dur_us) \
    spdlog::info("[TRACE tid={}] {} {} {}us", (tid), (svc), (op), (dur_us))

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
                engine_->onMachineInteracted(x, y, z, expected_block_id, player_id);
                publisher_->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_ACCEPTED),
                                            x, y, z, expected_block_id, 0, "Machine interacted",
                                            request_id);
                return;
            }
        }
        if (new_block_id == 0) {
            spdlog::warn("SetBlockCASHandler: cannot place air at ({},{},{})", x, y, z);
            publisher_->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_REJECTED),
                                        x, y, z, 0, 0, "Cannot place air",
                                        request_id);
            return;
        }
    }

    uint16_t final_block_id = (action_type == Protocol::PlayerActionType_LEFT_MOUSE_CLICK) ? 0 : new_block_id;
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
                                            request_id);
                return;
            }
            if (!contents.empty()) {
                inventoryStore_->setSlots(player_id, inv);
                spdlog::info("Multiblock contents returned to player {} on break at ({},{},{})",
                             player_id, x, y, z);
            }
        }
    }

    auto transform = applyBlockTransform(expected_block_id, final_block_id, final_meta);
    if (transform.has_value()) {
        final_block_id = transform->new_block_id;
        final_meta = transform->new_meta;
        spdlog::info("Block transformation applied: new_id={}", final_block_id);
    }

    publisher_->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_ACCEPTED),
        x, y, z, final_block_id, final_meta, nullptr, request_id);

    auto cas_t0 = std::chrono::steady_clock::now();
    repo_->setBlockCAS(x, y, z, expected_block_id, final_block_id, final_meta,
        [this, x, y, z, final_block_id, final_meta, expected_block_id, new_block_id, player_id, action_type, request_id, cas_t0](const CASResult& result) {
            auto cas_dur = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - cas_t0).count();
            TRACE_LOG(request_id, "cas_cb", "complete", cas_dur);
            auto processResult = [this, x, y, z, final_block_id, final_meta, expected_block_id, new_block_id, player_id, action_type, request_id](const CASResult& result) {
                if (result.status == 0) {
                    spdlog::info("Block CAS OK at ({},{},{}) final_id={}", x, y, z, final_block_id);

                    // Break: give the broken block to the player
                    if (action_type == Protocol::PlayerActionType_LEFT_MOUSE_CLICK) {
                        uint16_t broken_block = expected_block_id;
                        if (broken_block != 0 && onGiveItem_) {
                            spdlog::info("Giving block {} to player {}", broken_block, player_id);
                            onGiveItem_(player_id, broken_block, 1, -1);
                        }
                        if (onDrillUse_) {
                            onDrillUse_(player_id, x, y, z, broken_block);
                        }
                    }

                    // Place: consume the placed block from the player
                    if (action_type == Protocol::PlayerActionType_RIGHT_MOUSE_CLICK) {
                        uint16_t placed_block = new_block_id;
                        if (placed_block != 0 && onGiveItem_) {
                            spdlog::info("Placed block {} by player {}", placed_block, player_id);
                        }
                    }

                    publisher_->publishBlockChangedEvent(x, y, z, final_block_id, final_meta, request_id, player_id);
                    if (engine_) {
                        engine_->onBlockChanged(static_cast<uint32_t>(x),
                                                static_cast<uint32_t>(y),
                                                static_cast<uint32_t>(z),
                                                final_block_id, final_meta, 0);
                    }
                    if (action_type == Protocol::PlayerActionType_RIGHT_MOUSE_CLICK && onBlockPlaced_) {
                        onBlockPlaced_(player_id, x, y, z, final_block_id);
                    }
                } else { // CONFLICT
                    spdlog::warn("Block CAS CONFLICT at ({},{},{}) actual_id={}, from_id={}, to_id={}", x, y, z, result.block_id, expected_block_id, final_block_id);
                    publisher_->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_CONFLICT),
                                                x, y, z, result.block_id, result.meta, nullptr, request_id);
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