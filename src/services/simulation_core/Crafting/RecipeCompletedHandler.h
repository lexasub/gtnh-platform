#pragma once
#include <memory>
#include "Network/ITopicHandler.h"

namespace simcore {
class SimulationEngine;

class RecipeCompletedHandler : public ITopicHandler {
public:
    explicit RecipeCompletedHandler(std::shared_ptr<SimulationEngine> engine);
    void handle(const std::vector<uint8_t>& data) override;
private:
    std::shared_ptr<SimulationEngine> engine_;
};
} // namespace simcore
