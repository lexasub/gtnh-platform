#pragma once
#include "Network/ITopicHandler.h"
#include <memory>

namespace simcore {

class ContainerSessionRegistry;
class ChestStateManager;

// Handles a machine window close: persists the open machine session's live
// ECS slots (MachineState blob keyed by machine_id) then deregisters the
// per-player session.
class MachineCloseHandler : public ITopicHandler {
public:
  MachineCloseHandler(std::shared_ptr<ContainerSessionRegistry> sessions,
                      std::shared_ptr<ChestStateManager> stateMgr);
  void handle(const std::vector<uint8_t>& data) override;

private:
  std::shared_ptr<ContainerSessionRegistry> sessions_;
  std::shared_ptr<ChestStateManager> stateMgr_;
};

} // namespace simcore
