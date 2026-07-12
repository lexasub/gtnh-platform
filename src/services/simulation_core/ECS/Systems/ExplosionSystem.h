#pragma once

#include "../../Network/IEventPublisher.h"
#include "ISystem.h"
#include <entt/entt.hpp>
#include <memory>
#include <vector>

namespace simcore {

class ExplosionSystem : public ISystem {
public:
  ExplosionSystem(entt::registry &reg, std::shared_ptr<IEventPublisher> events)
      : reg_(reg), events_(events) {}

  void tick(float /*dt*/) override;

private:
  entt::registry &reg_;
  std::shared_ptr<IEventPublisher> events_;
};

} // namespace simcore
