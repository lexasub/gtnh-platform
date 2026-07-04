#pragma once
#include "../../Network/FluidClient.h"
#include "../../Network/ITopicHandler.h"
#include <entt/entt.hpp>
#include <memory>

namespace simcore {

class FluidFlowHandler : public ITopicHandler {
public:
  FluidFlowHandler(entt::registry &reg,
                   std::shared_ptr<FluidClient> fluidClient);

  void handle(const std::vector<uint8_t> &data) override;

private:
  entt::registry &reg_;
  std::shared_ptr<FluidClient> fluidClient_;
};

} // namespace simcore
