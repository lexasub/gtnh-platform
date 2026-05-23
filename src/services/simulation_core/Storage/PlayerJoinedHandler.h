#pragma once
#include <memory>
#include "../Network/ITopicHandler.h"
namespace simcore {
class PlayerInventoryStore;
class PlayerJoinedHandler : public ITopicHandler {
public:
    explicit PlayerJoinedHandler(std::shared_ptr<PlayerInventoryStore> inv);
    void handle(const std::vector<uint8_t>& data) override;
private:
    std::shared_ptr<PlayerInventoryStore> inventoryStore_;
};
} // namespace simcore
