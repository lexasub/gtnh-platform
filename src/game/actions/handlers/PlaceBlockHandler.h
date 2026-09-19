#pragma once
#include <cstdint>

namespace simcore {

class ActionContext;

// Right-click placement: place the held block on the face-adjacent cell
// (transform rules apply), consume it from the player inventory on success
// and fire the block-placed hook.
class PlaceBlockHandler {
public:
  bool canHandle(const ActionContext& ctx) const;
  void handle(const ActionContext& ctx) const;
};

} // namespace simcore
