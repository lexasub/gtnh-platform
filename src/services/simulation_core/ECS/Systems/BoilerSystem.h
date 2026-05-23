#pragma once

#include <entt/entt.hpp>
#include <memory>
#include "ISystem.h"
#include "../components/MachineComponent.h"
#include "../components/InventoryContainer.h"
#include "../components/EnergyStorage.h"
#include "../components/RecipeProgress.h"
#include "../components/HeatIntakeComponent.h"
#include "../../Network/IEventPublisher.h"
#include "../../Network/PipeEnergyClient.h"
#include "MachineRegistry.h"

namespace simcore {

class PipeEnergyClient;

class BoilerSystem : public ISystem {
public:
    BoilerSystem(entt::registry& reg,
                 std::shared_ptr<IEventPublisher> events,
                 std::shared_ptr<PipeEnergyClient> pipeClient);

    void tick(float dt) override;

private:
    entt::registry& reg_;
    std::shared_ptr<IEventPublisher> events_;
    std::shared_ptr<PipeEnergyClient> pipeClient_;
};

} // namespace simcore
