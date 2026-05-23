#pragma once

#include <entt/entt.hpp>
#include <memory>
#include "ISystem.h"
#include "../components/MachineComponent.h"
#include "../components/EnergyStorage.h"
#include "../components/Position.h"
#include "../components/HeatIntakeComponent.h"
#include "../components/OverheatComponent.h"
#include "../components/HeatSlowComponent.h"
#include "../../Network/IEventPublisher.h"
#include "HeatConstants.h"

namespace simcore {

class HeatTransferSystem : public ISystem {
public:
    HeatTransferSystem(entt::registry& reg,
                       MachineRegistry& machineRegistry,
                       std::shared_ptr<IEventPublisher> events);

    void tick(float dt) override;

private:
    entt::registry& reg_;
    MachineRegistry& machineRegistry_;
    std::shared_ptr<IEventPublisher> events_;
};

} // namespace simcore
