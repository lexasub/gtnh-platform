#pragma once

#include "Common/Inventory.h"
#include "Common/Types.h"
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
  explicit InteractionSystem(const IBlockQuery *blockQuery,
                             InventoryState *inventory = nullptr);

  void SetInventory(InventoryState *inventory) { inventory_ = inventory; }
  void SetBinder(const InputBinder *binder) { binder_ = binder; }

  // Ray-cast from camera, highlight target, dispatch break/place actions.
  // Must be called every frame AFTER camera is updated.
  void Update(const Camera &camera, const InputState &input, World &world,
              NetClient &netClient);

  // Fresh ray-cast — returns the targeted block without mutating internal
  // state. Used by GameClient to check for block UI opening on right-click.
  BlockPos RaycastTarget(const Camera &camera) const;

  // Fresh ray-cast FROM A MOUSE PIXEL (un-project through view/proj) — returns
  // the block under the cursor, not the block at screen center. Used by the
  // wrench overlay so a click on a connection bar targets the bar's own block.
  BlockPos RaycastTargetAtMouse(const Camera &camera, float width, float height,
                                double mouseX, double mouseY) const;

  // GT-style wrench hit: ray from the mouse pixel, returns the hit block, the
  // entered face (sideHit) and local UV on that face — input to
  // determineWrenchingSide.
  renderlib::Raycaster::HitInfo RaycastHitAtMouse(const Camera &camera,
                                                  float width, float height,
                                                  double mouseX,
                                                  double mouseY) const;
  // Ray-cast from the screen CENTER (crosshair). The mouse is captured
  // (GLFW_CURSOR_DISABLED) while the UI is closed, so the cursor's virtual
  // position is not the screen center; a mouse-pixel ray would miss the
  // targeted pipe. GT-style side selection is screen-center driven: hit.u/v
  // select the 3x3 grid cell on the entered face.
  renderlib::Raycaster::HitInfo RaycastHitAtCenter(const Camera &camera,
                                                   float width,
                                                   float height) const;

  // Face of the targeted block the player is looking at (0=DOWN..5=EAST).
  uint8_t TargetFace(const Camera &camera) const;

  // Item id in the currently selected hotbar slot (0 = empty).
  uint16_t GetHeldItem() const;

  bool HasHighlight() const { return hasHighlight_; }
  BlockPos GetHighlightedBlock() const { return highlightedBlock_; }

private:
  Ray buildRay(const Camera &camera) const;
  Ray buildRayFromMouse(const Camera &camera, float width, float height,
                        double mouseX, double mouseY) const;

  renderlib::Raycaster raycaster_;
  InventoryState *inventory_ = nullptr;
  const InputBinder *binder_ = nullptr;
  BlockPos highlightedBlock_{};
  bool hasHighlight_ = false;
};
