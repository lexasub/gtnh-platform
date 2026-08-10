#pragma once
#include "ITopicHandler.h"
#include "../Crafting/WorkbenchStateManager.h"
#include "IEventPublisher.h"
#include <memory>
#include <vector>

namespace simcore {

class ContainerSessionRegistry;
class PlayerInventoryStore;
class IoUringRouterClient;

class WorkbenchOpenHandler : public ITopicHandler {
public:
    WorkbenchOpenHandler(std::shared_ptr<ContainerSessionRegistry> chestSessions,
                         std::shared_ptr<PlayerInventoryStore> inventoryStore,
                         std::shared_ptr<IoUringRouterClient> router,
                         std::shared_ptr<simulation_core::WorkbenchStateManager> wbStateManager,
                         std::shared_ptr<IEventPublisher> eventPublisher)
        : chestSessions_(std::move(chestSessions))
        , inventoryStore_(std::move(inventoryStore))
        , router_(std::move(router))
        , wbStateManager_(std::move(wbStateManager))
        , eventPublisher_(std::move(eventPublisher)) {}

    void handle(const std::vector<uint8_t>& data) override;

private:
    std::shared_ptr<ContainerSessionRegistry> chestSessions_;
    std::shared_ptr<PlayerInventoryStore> inventoryStore_;
    std::shared_ptr<IoUringRouterClient> router_;
    std::shared_ptr<simulation_core::WorkbenchStateManager> wbStateManager_;
    std::shared_ptr<IEventPublisher> eventPublisher_;
};

} // namespace simcore
