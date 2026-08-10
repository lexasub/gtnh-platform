#pragma once
#include "../Network/ITopicHandler.h"
#include <memory>

namespace simcore {
class IoUringRouterClient;
class PlayerInventoryStore;
class QuestManager;
class IEventPublisher;
class MainThreadQueue;
} // namespace simcore

namespace RecipeManager {
class RecipeManager;
struct ItemStack;
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
      std::shared_ptr<WorkbenchStateManager> wbStateManager,
      simcore::MainThreadQueue* mainQueue,
      std::shared_ptr<simcore::IEventPublisher> eventPublisher);
  void handle(const std::vector<uint8_t> &data) override;

private:
  // Core craft logic: resolves recipe from authoritative grid, consumes inputs,
  // returns result. Extracted so it can run inline (cache hit) or via main-queue
  // bounce (ESS async callback).
  void doCraft(uint64_t playerId, int32_t x, int32_t y, int32_t z,
               const std::vector<RecipeManager::ItemStack> &grid);

  std::shared_ptr<simcore::IoUringRouterClient> router_;
  std::shared_ptr<RecipeManager::RecipeManager> recipeManager_;
  std::shared_ptr<simcore::PlayerInventoryStore> inventoryStore_;
  std::shared_ptr<simcore::QuestManager> questManager_;
  std::shared_ptr<WorkbenchStateManager> wbStateManager_;
  simcore::MainThreadQueue* mainQueue_ = nullptr;
  std::shared_ptr<simcore::IEventPublisher> eventPublisher_;
};
} // namespace simulation_core
