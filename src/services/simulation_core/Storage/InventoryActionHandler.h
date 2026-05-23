#pragma once
#include <memory>
#include "../Network/ITopicHandler.h"
namespace simcore {
class PlayerInventoryStore; class IoUringRouterClient;
class InventoryActionHandler : public ITopicHandler {
public:
    InventoryActionHandler(std::shared_ptr<PlayerInventoryStore> inv, std::shared_ptr<IoUringRouterClient> r);
    void handle(const std::vector<uint8_t>& data) override;
private:
    std::shared_ptr<PlayerInventoryStore> inventoryStore_;
    std::shared_ptr<IoUringRouterClient> router_;
};
} // namespace simcore
