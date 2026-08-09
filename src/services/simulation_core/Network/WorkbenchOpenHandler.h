#pragma once
#include "ITopicHandler.h"
#include "../Crafting/WorkbenchStateManager.h"
#include "IEventPublisher.h"
#include <vector>

namespace simcore {

class WorkbenchOpenHandler : public ITopicHandler {
public:
    WorkbenchOpenHandler(std::shared_ptr<simulation_core::WorkbenchStateManager> wbStateManager,
                        std::shared_ptr<IEventPublisher> eventPublisher)
        : wbStateManager_(wbStateManager), eventPublisher_(eventPublisher) {}

    void handle(const std::vector<uint8_t>& data) override;

private:
    std::shared_ptr<simulation_core::WorkbenchStateManager> wbStateManager_;
    std::shared_ptr<IEventPublisher> eventPublisher_;
};

} // namespace simcore
