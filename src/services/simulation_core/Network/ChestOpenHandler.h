#pragma once
#include "Network/ITopicHandler.h"
#include <memory>

namespace simcore {

class ContainerSessionRegistry;
class ChestStateManager;
class IoUringRouterClient;
class PlayerInventoryStore;

// Handles a chest window opening: loads the saved chest slots (async via
// ChestStateManager → EntityStateStore), registers the per-player container
// session, and publishes the full InventoryUpdate snapshot (container_id=1)
// back to the client.
class ChestOpenHandler : public ITopicHandler {
public:
  ChestOpenHandler(std::shared_ptr<ContainerSessionRegistry> sessions,
                   std::shared_ptr<ChestStateManager> stateMgr,
                   std::shared_ptr<PlayerInventoryStore> inv,
                   std::shared_ptr<IoUringRouterClient> router);
  void handle(const std::vector<uint8_t>& data) override;

private:
  std::shared_ptr<ContainerSessionRegistry> sessions_;
  std::shared_ptr<ChestStateManager> stateMgr_;
  std::shared_ptr<PlayerInventoryStore> inv_;
  std::shared_ptr<IoUringRouterClient> router_;
};

} // namespace simcore
