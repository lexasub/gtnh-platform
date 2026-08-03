#pragma once
#include "../Network/ITopicHandler.h"
#include <memory>
namespace simcore {
class PlayerInventoryStore;
class IoUringRouterClient;
class QuestManager;
class PlayerJoinedHandler : public ITopicHandler {
public:
  PlayerJoinedHandler(std::shared_ptr<PlayerInventoryStore> inv,
                      std::shared_ptr<IoUringRouterClient> router,
                      std::shared_ptr<QuestManager> questManager);
  void handle(const std::vector<uint8_t> &data) override;

private:
  std::shared_ptr<PlayerInventoryStore> inventoryStore_;
  std::shared_ptr<IoUringRouterClient> router_;
  std::shared_ptr<QuestManager> questManager_;
};
} // namespace simcore
