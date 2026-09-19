#pragma once
#include "../../Network/ITopicHandler.h"
#include "../../Network/PipeEnergyClient.h"
#include <entt/entt.hpp>
#include <memory>

namespace simcore {

class EnergyFlowHandler : public ITopicHandler {
public:
  EnergyFlowHandler(entt::registry &reg,
                    std::shared_ptr<PipeEnergyClient> pipeClient);

  void handle(const std::vector<uint8_t> &data) override;

private:
  entt::registry &reg_;
  std::shared_ptr<PipeEnergyClient> pipeClient_;
};

} // namespace simcore
