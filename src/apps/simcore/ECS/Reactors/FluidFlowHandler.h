#pragma once
#include "../../Network/ITopicHandler.h"
#include "../../Network/ResourceBufferStatePublisher.h"
#include <entt/entt.hpp>
#include <memory>

namespace simcore {

class FluidFlowHandler : public ITopicHandler {
public:
  FluidFlowHandler(entt::registry &reg,
                   std::shared_ptr<ResourceBufferStatePublisher> statePublisher);

  void handle(const std::vector<uint8_t> &data) override;

private:
  entt::registry &reg_;
  std::shared_ptr<ResourceBufferStatePublisher> statePublisher_;
};

} // namespace simcore
