#pragma once
#include <memory>
#include "../../Network/ITopicHandler.h"
#include "../../Network/FluidClient.h"
#include <entt/entt.hpp>

namespace simcore {

class FluidFlowHandler : public ITopicHandler {
public:
    FluidFlowHandler(entt::registry& reg,
                     std::shared_ptr<FluidClient> fluidClient);

    void handle(const std::vector<uint8_t>& data) override;

private:
    entt::registry& reg_;
    std::shared_ptr<FluidClient> fluidClient_;
};

} // namespace simcore
