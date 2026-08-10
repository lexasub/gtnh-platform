#pragma once
#include "../Network/ITopicHandler.h"
#include <memory>

namespace simulation_core {
class WorkbenchStateManager;
}

namespace simcore {
class PlayerInventoryStore;
class IoUringRouterClient;
class ContainerSessionRegistry;
class ChestStateManager;
class QuestManager;
class InventoryActionHandler : public ITopicHandler {
public:
  InventoryActionHandler(std::shared_ptr<PlayerInventoryStore> inv,
                         std::shared_ptr<IoUringRouterClient> r,
                         std::shared_ptr<ContainerSessionRegistry> chestSessions,
                         std::shared_ptr<ChestStateManager> chestStateManager,
                         std::shared_ptr<QuestManager> questManager,
                         std::shared_ptr<simulation_core::WorkbenchStateManager> wbStateManager);
  void handle(const std::vector<uint8_t> &data) override;

private:
  std::shared_ptr<PlayerInventoryStore> inventoryStore_;
  std::shared_ptr<IoUringRouterClient> router_;
  std::shared_ptr<ContainerSessionRegistry> chestSessions_;
  std::shared_ptr<ChestStateManager> sessionsStateMgr_;
  std::shared_ptr<QuestManager> questManager_;
  std::shared_ptr<simulation_core::WorkbenchStateManager> wbStateManager_;
};
} // namespace simcore
