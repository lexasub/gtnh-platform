#pragma once
#include <memory>
#include "Network/ITopicHandler.h"

namespace simcore {
class IoUringRouterClient;
class PlayerInventoryStore;
} // namespace simcore

namespace RecipeManager {
class RecipeManager;
} // namespace RecipeManager

namespace simcore {

class CraftRequestHandler : public ITopicHandler {
public:
    CraftRequestHandler(std::shared_ptr<IoUringRouterClient> router,
                        std::shared_ptr<RecipeManager::RecipeManager> recipeManager,
                        std::shared_ptr<PlayerInventoryStore> inventoryStore);
    void handle(const std::vector<uint8_t>& data) override;
private:
    std::shared_ptr<IoUringRouterClient> router_;
    std::shared_ptr<RecipeManager::RecipeManager> recipeManager_;
    std::shared_ptr<PlayerInventoryStore> inventoryStore_;
};
} // namespace simcore
