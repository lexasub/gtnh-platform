#include "Actions/SetBlockCASHandler.h"
#include "Storage/IBlockRepository.h"
#include "Network/IEventPublisher.h"
#include "ECS/SimulationEngine.h"
#include "World/BlockTransforms.h"
#include "core_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

namespace simcore {

SetBlockCASHandler::SetBlockCASHandler(std::shared_ptr<IBlockRepository> repo,
                                       std::shared_ptr<IEventPublisher> publisher,
                                       std::shared_ptr<SimulationEngine> engine,
                                       ItemGiveCallback onGiveItem,
                                       DrillUseCallback onDrillUse)
    : repo_(std::move(repo))
    , publisher_(std::move(publisher))
    , engine_(std::move(engine))
    , onGiveItem_(std::move(onGiveItem))
    , onDrillUse_(std::move(onDrillUse))
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

    if (action_type == Protocol::PlayerActionType_RIGHT_MOUSE_CLICK && new_block_id == 0) {
        if (engine_) {
            auto* machineReg = engine_->getMachineRegistry();
            if (machineReg && machineReg->IsMachine(expected_block_id)) {
                engine_->onMachineInteracted(x, y, z, expected_block_id, player_id);
                publisher_->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_ACCEPTED),
                                            x, y, z, expected_block_id, 0, "Machine interacted");
                return;
            }
        }
        spdlog::warn("SetBlockCASHandler: cannot place air at ({},{},{})", x, y, z);
        publisher_->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_REJECTED),
                                    x, y, z, 0, 0, "Cannot place air");
        return;
    }

    uint16_t final_block_id = (action_type == Protocol::PlayerActionType_LEFT_MOUSE_CLICK) ? 0 : new_block_id;
    uint8_t final_meta = 0;

    auto transform = applyBlockTransform(expected_block_id, final_block_id, final_meta);
    if (transform.has_value()) {
        final_block_id = transform->new_block_id;
        final_meta = transform->new_meta;
        spdlog::info("Block transformation applied: new_id={}", final_block_id);
    }

    publisher_->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_ACCEPTED),
        x, y, z, final_block_id, final_meta, nullptr);

    repo_->setBlockCAS(x, y, z, expected_block_id, final_block_id, final_meta,
        [this, x, y, z, final_block_id, final_meta, expected_block_id, new_block_id, player_id, action_type](const CASResult& result) {
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
                        // Remove one by giving negative count (consumed on client side)
                        // For now this is handled implicitly — client optimistically consumes
                        spdlog::info("Placed block {} by player {}", placed_block, player_id);
                    }
                }

                publisher_->publishBlockChangedEvent(x, y, z, final_block_id, final_meta);
                if (engine_) {
                    engine_->onBlockChanged(static_cast<uint32_t>(x),
                                            static_cast<uint32_t>(y),
                                            static_cast<uint32_t>(z),
                                            final_block_id, final_meta, 0);
                }
            } else { // CONFLICT
                spdlog::warn("Block CAS CONFLICT at ({},{},{}) actual_id={}, from_id={}, to_id={}", x, y, z, result.block_id, expected_block_id, final_block_id);
                publisher_->publishBlockAck(static_cast<uint8_t>(Protocol::BlockAckStatus_CONFLICT),
                                            x, y, z, result.block_id, result.meta, nullptr);
            }
        });
}

} // namespace simcore