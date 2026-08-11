#include "World/InteractionSystem.h"
#include "Camera/Camera.h"
#include "Common/InputState.h"
#include "Network/NetClient.h"
#include "World/World.h"
#include "UI/Core/InputBinder.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
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

// Un-project a mouse pixel into a world-space ray: origin = camera, dir = the
// point on the far plane under the cursor. Mirrors how the overlay projects
// block corners (same view/proj), so the clickable bars line up with the ray.
Ray InteractionSystem::buildRayFromMouse(const Camera& camera, float width,
                                         float height, double mouseX,
                                         double mouseY) const {
    const float aspect = width / height;
    const glm::mat4 view = camera.GetViewMatrix();
    const glm::mat4 proj = camera.GetProjectionMatrix(aspect);
    const glm::mat4 invVP = glm::inverse(proj * view);

    // NDC: x,y from mouse; z=0 near, z=1 far.
    const float ndcX = static_cast<float>(mouseX) / width * 2.0f - 1.0f;
    const float ndcY = 1.0f - static_cast<float>(mouseY) / height * 2.0f;

    glm::vec4 nearP = invVP * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    glm::vec4 farP  = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    if (std::abs(nearP.w) < 1e-6f || std::abs(farP.w) < 1e-6f)
        return {camera.GetRayOrigin(), camera.GetForward()};
    nearP /= nearP.w;
    farP  /= farP.w;

    const glm::vec3 origin = camera.GetRayOrigin();
    const glm::vec3 dir = glm::normalize(glm::vec3(farP - nearP));
    return {origin, dir};
}

BlockPos InteractionSystem::RaycastTargetAtMouse(const Camera& camera,
                                                 float width, float height,
                                                 double mouseX,
                                                 double mouseY) const {
    Ray ray = buildRayFromMouse(camera, width, height, mouseX, mouseY);
    return raycaster_.GetTargetedBlock(ray, renderlib::Raycaster::REACH_DIST);
}

renderlib::Raycaster::HitInfo InteractionSystem::RaycastHitAtMouse(
    const Camera& camera, float width, float height, double mouseX,
    double mouseY) const {
    Ray ray = buildRayFromMouse(camera, width, height, mouseX, mouseY);
    return raycaster_.RaycastHit(ray, renderlib::Raycaster::REACH_DIST);
}

// Ray-cast from the CENTER of the screen (crosshair). The mouse is captured
// (GLFW_CURSOR_DISABLED) while the UI is closed, so the cursor's virtual
// position is NOT the screen center — clicking with a mouse-pixel ray
// frequently misses the targeted pipe. GT-style side selection is
// screen-center driven: the crosshair hits the faced face, and hit.u/v
// select the 3x3 grid cell.
renderlib::Raycaster::HitInfo InteractionSystem::RaycastHitAtCenter(
    const Camera& camera, float width, float height) const {
    Ray ray = buildRayFromMouse(camera, width, height, width * 0.5,
                                height * 0.5);
    return raycaster_.RaycastHit(ray, renderlib::Raycaster::REACH_DIST);
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
                // Same as GameClient's right-click wrench: the G-key toggles
                // the face the camera is looking at (the near face). The far
                // face is reachable by clicking a grid corner on the overlay.
                TargetFace(camera),
                heldItem
            );
        }
        // (InputState doesn't track key edges, so we send each frame the key is held;
        //  the server should deduplicate based on player action cooldown)
    }
}
