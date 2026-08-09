#pragma once
#include "../Network/ITopicHandler.h"
#include <memory>

namespace simcore {
class IoUringRouterClient;
class PlayerInventoryStore;
class QuestManager;
} // namespace simcore

namespace RecipeManager {
class RecipeManager;
} // namespace RecipeManager

namespace simulation_core {
class WorkbenchStateManager;
class CraftRequestHandler : public simcore::ITopicHandler {
public:
  CraftRequestHandler(
      std::shared_ptr<simcore::IoUringRouterClient> router,
      std::shared_ptr<RecipeManager::RecipeManager> recipeManager,
      std::shared_ptr<simcore::PlayerInventoryStore> inventoryStore,
      std::shared_ptr<simcore::QuestManager> questManager,
      std::shared_ptr<WorkbenchStateManager> wbStateManager);
  void handle(const std::vector<uint8_t> &data) override;

private:
  std::shared_ptr<simcore::IoUringRouterClient> router_;
  std::shared_ptr<RecipeManager::RecipeManager> recipeManager_;
  std::shared_ptr<simcore::PlayerInventoryStore> inventoryStore_;
  std::shared_ptr<simcore::QuestManager> questManager_;
  std::shared_ptr<WorkbenchStateManager> wbStateManager_;
};
} // namespace simulation_core
