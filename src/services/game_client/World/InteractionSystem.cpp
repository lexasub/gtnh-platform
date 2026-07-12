#include "World/InteractionSystem.h"
#include "Camera/Camera.h"
#include "Common/InputState.h"
#include "Network/NetClient.h"
#include "World/World.h"
#include "UI/Core/InputBinder.h"
#include <spdlog/spdlog.h>
#include <limits>
#include <cstdint>
#include "core_generated.h"

InteractionSystem::InteractionSystem(const IBlockQuery* blockQuery,
                                     InventoryState* inventory)
    : raycaster_(blockQuery), inventory_(inventory) {}

Ray InteractionSystem::buildRay(const Camera& camera) const {
    return {camera.GetRayOrigin(), camera.GetForward()};
}

BlockPos InteractionSystem::RaycastTarget(const Camera& camera) const {
    Ray ray = buildRay(camera);
    return raycaster_.GetTargetedBlock(ray, renderlib::Raycaster::REACH_DIST);
}

uint16_t InteractionSystem::getSelectedBlockId() const {
    if (!inventory_ || inventory_->selectedSlot < 0 ||
        static_cast<size_t>(inventory_->selectedSlot) >= inventory_->slots.size()) {
        return 0;
    }
    return inventory_->slots[inventory_->selectedSlot].item_id;
}

void InteractionSystem::consumeSelectedSlot() {
    if (!inventory_ || inventory_->selectedSlot < 0 ||
        static_cast<size_t>(inventory_->selectedSlot) >= inventory_->slots.size()) {
        return;
    }
    auto& slot = inventory_->slots[inventory_->selectedSlot];
    if (slot.count > 0) {
        slot.count--;
        if (slot.count == 0) {
            slot.item_id = 0;
            slot.meta = 0;
        }
    }
}

void InteractionSystem::Update(const Camera& camera, const InputState& input,
                                World& world, NetClient& netClient) {
    Ray ray = buildRay(camera);
    BlockPos target = raycaster_.GetTargetedBlock(ray, renderlib::Raycaster::REACH_DIST);

    hasHighlight_ = target.x != std::numeric_limits<int32_t>::max();
    if (hasHighlight_) {
        highlightedBlock_ = target;
    }

    uint64_t player_id = inventory_ ? inventory_->player_id : 0;

    // Left-click: break block
    if (input.mouseLeftPressed && hasHighlight_) {
        // Debounce: skip if action already in-flight for this position
        if (!world.IsBlockActionPending(highlightedBlock_)) {
            auto currentBlockType = world.GetBlockAt(highlightedBlock_);
            spdlog::info("Left click at ({},{},{})", highlightedBlock_.x,
                         highlightedBlock_.y, highlightedBlock_.z);
            netClient.SendBlockAction(
                Protocol::PlayerActionType::PlayerActionType_LEFT_MOUSE_CLICK,
                highlightedBlock_.x, highlightedBlock_.y, highlightedBlock_.z,
                currentBlockType, static_cast<uint16_t>(0),
                0, player_id);
            world.MarkBlockActionSent(highlightedBlock_);
        }
    }

    // Wrench cycle on highlighted block (key from held binding "wrench_cycle")
    if (binder_ && binder_->IsHeld("wrench_cycle", input) && hasHighlight_) {
        // Detect which face the player is looking at via raycast
        int faceX = 0, faceY = 0, faceZ = 0;
        raycaster_.GetTargetedBlock(ray, renderlib::Raycaster::REACH_DIST,
                                    &faceX, &faceY, &faceZ);
        uint8_t hitFace = 0; // DOWN (default fallback)
        if      (faceY == -1) hitFace = 0; // DOWN
        else if (faceY ==  1) hitFace = 1; // UP
        else if (faceZ == -1) hitFace = 2; // NORTH
        else if (faceZ ==  1) hitFace = 3; // SOUTH
        else if (faceX == -1) hitFace = 4; // WEST
        else if (faceX ==  1) hitFace = 5; // EAST

        netClient.SendToolAction(
            player_id,
            Protocol::ToolActionType::ToolActionType_WRENCH_CYCLE,
            highlightedBlock_.x, highlightedBlock_.y, highlightedBlock_.z,
            hitFace
        );
        // (InputState doesn't track key edges, so we send each frame the key is held;
        //  the server should deduplicate based on player action cooldown)
    }

    // Right-click: place block or interact with machine
    if (input.mouseRightPressed) {
        BlockPos placePos = raycaster_.GetPlacementPos(ray);
        if (placePos.x != std::numeric_limits<int32_t>::max()) {
            if (!world.IsBlockActionPending(placePos)) {
                auto currentBlockType = world.GetBlockAt(placePos);
                uint16_t placedBlockId = getSelectedBlockId();
                spdlog::info("Right click at ({},{},{}) existing={} placed={}",
                             placePos.x, placePos.y, placePos.z,
                             currentBlockType, placedBlockId);
                netClient.SendBlockAction(
                    Protocol::PlayerActionType::PlayerActionType_RIGHT_MOUSE_CLICK,
                    placePos.x, placePos.y, placePos.z,
                    currentBlockType, placedBlockId,
                    0, player_id);
                world.MarkBlockActionSent(placePos);
                if (placedBlockId != 0) { // TODO - consume wen simcored say us that need consume, because we have many cases withot consuming item
                    consumeSelectedSlot();
                }
            }
        }
    }
}
