#pragma once

#include <entt/entt.hpp>
#include <memory>
#include <vector>
#include "ISystem.h"
#include "../components/MachineComponent.h"
#include "../components/Position.h"
#include "../components/OverheatComponent.h"
#include "../components/HeatSlowComponent.h"
#include "HeatConstants.h"
#include "../../Network/IEventPublisher.h"

namespace simcore {

class ExplosionSystem : public ISystem {
public:
    ExplosionSystem(entt::registry& reg,
                   std::shared_ptr<IEventPublisher> events)
        : reg_(reg), events_(events) {}

    void tick(float /*dt*/) override;

private:
    entt::registry& reg_;
    std::shared_ptr<IEventPublisher> events_;
};

} // namespace simcore
