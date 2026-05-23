#pragma once
#include <memory>
#include "../Network/ITopicHandler.h"
namespace simcore {
class SimulationEngine; class PlayerInventoryStore; class IoUringRouterClient;
class ToolActionHandler : public ITopicHandler {
public:
    ToolActionHandler(std::shared_ptr<SimulationEngine> engine, std::shared_ptr<PlayerInventoryStore> inventoryStore, std::shared_ptr<IoUringRouterClient> router);
    void handle(const std::vector<uint8_t>& data) override;
private:
    std::shared_ptr<SimulationEngine> engine_;
    std::shared_ptr<PlayerInventoryStore> inventoryStore_;
    std::shared_ptr<IoUringRouterClient> router_;
};
} // namespace simcore
