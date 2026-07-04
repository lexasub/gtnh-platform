#pragma once
#include "../Network/ITopicHandler.h"
#include <memory>
namespace simcore {
class SimulationEngine;
class PlayerInventoryStore;
class EntityStateStoreClient;
class IEventPublisher;
class IoUringRouterClient;
class MachineSlotHandler : public ITopicHandler {
public:
  MachineSlotHandler(std::shared_ptr<SimulationEngine> engine,
                     std::shared_ptr<PlayerInventoryStore> inv,
                     std::shared_ptr<EntityStateStoreClient> ess,
                     std::shared_ptr<IEventPublisher> events,
                     std::shared_ptr<IoUringRouterClient> router);
  void handle(const std::vector<uint8_t> &data) override;

private:
  std::shared_ptr<SimulationEngine> engine_;
  std::shared_ptr<PlayerInventoryStore> inventoryStore_;
  std::shared_ptr<EntityStateStoreClient> entityState_;
  std::shared_ptr<IEventPublisher> events_;
  std::shared_ptr<IoUringRouterClient> router_;
};
} // namespace simcore
