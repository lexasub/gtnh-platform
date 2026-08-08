#pragma once
#include <cstdint>

namespace simcore {

class ActionContext;

// Left-click block break: multiblock break guard (contents must fit the
// player inventory or the break is refused), CAS the block to air, give the
// broken block and charge the drill.
class BreakBlockHandler {
public:
  bool canHandle(const ActionContext& ctx) const;
  void handle(const ActionContext& ctx) const;
};

} // namespace simcore
