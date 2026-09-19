#pragma once
#include <cstdint>

namespace simcore {

class ActionContext;

// Right-click on a machine: lazily create its ECS entity when the block
// predates this simcore instance, report real energy state, open the window.
// Left-click on a machine flagged interact_on_left (e.g. rotare_generator):
// perform the machine interaction instead of breaking the block.
class MachineInteractHandler {
public:
  bool canHandle(const ActionContext& ctx) const;
  void handle(const ActionContext& ctx) const;
};

} // namespace simcore
