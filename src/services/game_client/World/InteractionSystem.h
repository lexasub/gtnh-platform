#pragma once

#include "Common/Types.h"
#include "Common/Inventory.h"
#include "RenderLib/Utils/Raycaster.h"

class Camera;
class InputBinder;
class InputState;
class World;
class NetClient;

// Handles player world interaction: ray-casting, block highlighting,
// block break on left-click, block place on right-click.
// Stateless per-frame — call Update() once per game tick.
class InteractionSystem {
public:
    explicit InteractionSystem(const IBlockQuery* blockQuery,
                               InventoryState* inventory = nullptr);

    void SetInventory(InventoryState* inventory) { inventory_ = inventory; }
    void SetBinder(const InputBinder* binder) { binder_ = binder; }

    // Ray-cast from camera, highlight target, dispatch break/place actions.
    // Must be called every frame AFTER camera is updated.
    void Update(const Camera& camera, const InputState& input,
                World& world, NetClient& netClient);

    // Fresh ray-cast — returns the targeted block without mutating internal state.
    // Used by GameClient to check for block UI opening on right-click.
    BlockPos RaycastTarget(const Camera& camera) const;

    bool HasHighlight() const { return hasHighlight_; }
    BlockPos GetHighlightedBlock() const { return highlightedBlock_; }

private:
    Ray buildRay(const Camera& camera) const;

    uint16_t getSelectedBlockId() const;
    void consumeSelectedSlot();

    renderlib::Raycaster raycaster_;
    InventoryState* inventory_ = nullptr;
    const InputBinder* binder_ = nullptr;
    BlockPos highlightedBlock_{};
    bool hasHighlight_ = false;
};
