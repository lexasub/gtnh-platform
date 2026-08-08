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
#include <data/registry/ToolIds.h>

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

uint16_t InteractionSystem::GetHeldItem() const {
    if (!inventory_ || inventory_->selectedSlot < 0 ||
        static_cast<size_t>(inventory_->selectedSlot) >= inventory_->slots.size()) {
        return 0;
    }
    return inventory_->slots[inventory_->selectedSlot].item_id;
}

uint8_t InteractionSystem::TargetFace(const Camera& camera) const {
    Ray ray = buildRay(camera);
    int faceX = 0, faceY = 0, faceZ = 0;
    raycaster_.GetTargetedBlock(ray, renderlib::Raycaster::REACH_DIST,
                                &faceX, &faceY, &faceZ);
    if      (faceY == -1) return 0; // DOWN
    else if (faceY ==  1) return 1; // UP
    else if (faceZ == -1) return 2; // NORTH
    else if (faceZ ==  1) return 3; // SOUTH
    else if (faceX == -1) return 4; // WEST
    else if (faceX ==  1) return 5; // EAST
    return 0;
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
                currentBlockType, GetHeldItem(),
                0, player_id);
            world.MarkBlockActionSent(highlightedBlock_);
        }
    }

    // Wrench cycle on highlighted block (key from held binding "wrench_cycle")
    if (binder_ && binder_->IsHeld("wrench_cycle", input) && hasHighlight_) {
        uint16_t heldItem = GetHeldItem();
        // Only send if player holds a wrench
        if (heldItem != ITEM_WRENCH) {
            spdlog::trace("G held but not a wrench (itemId={})", heldItem);
        } else {
            netClient.SendToolAction(
                player_id,
                Protocol::ToolActionType::ToolActionType_WRENCH_CYCLE,
                highlightedBlock_.x, highlightedBlock_.y, highlightedBlock_.z,
                TargetFace(camera),
                heldItem
            );
        }
        // (InputState doesn't track key edges, so we send each frame the key is held;
        //  the server should deduplicate based on player action cooldown)
    }
}
