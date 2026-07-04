#pragma once

#include "../../Network/IEventPublisher.h"
#include "../components/Block.h"
#include "../components/EnergyStorage.h"
#include "../components/Position.h"
#include "../components/TransformerComponent.h"
#include "ISystem.h"
#include "Network/PipeEnergyClient.h"
#include <entt/entt.hpp>

namespace simcore {

class PipeEnergyClient;

class TransformerSystem : public ISystem {
public:
  TransformerSystem(entt::registry &reg,
                    std::shared_ptr<IEventPublisher> events,
                    std::shared_ptr<PipeEnergyClient> pipeClient);

  void tick(float dt) override;

  static bool isTransformer(uint16_t block_id);

private:
  entt::registry &reg_;
  std::shared_ptr<IEventPublisher> events_;
  std::shared_ptr<PipeEnergyClient> pipeClient_;
};

} // namespace simcore
