#pragma once
#include "Network/ITopicHandler.h"
#include <memory>

namespace simcore {

class ContainerSessionRegistry;
class ChestStateManager;

// Handles a chest window close: persists the open container session's slots
// (server-authoritative, via ChestStateManager → EntityStateStore) then
// deregisters the per-player session.
class ChestCloseHandler : public ITopicHandler {
public:
  ChestCloseHandler(std::shared_ptr<ContainerSessionRegistry> sessions,
                    std::shared_ptr<ChestStateManager> stateMgr);
  void handle(const std::vector<uint8_t>& data) override;

private:
  std::shared_ptr<ContainerSessionRegistry> sessions_;
  std::shared_ptr<ChestStateManager> stateMgr_;
};

} // namespace simcore
