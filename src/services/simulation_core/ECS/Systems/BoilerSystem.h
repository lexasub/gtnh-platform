#pragma once

#include "../../Network/IEventPublisher.h"
#include "../../Network/PipeEnergyClient.h"
#include "../components/EnergyStorage.h"
#include "../components/HeatIntakeComponent.h"
#include "../components/InventoryContainer.h"
#include "../components/MachineComponent.h"
#include "../components/RecipeProgress.h"
#include "ISystem.h"
#include "MachineRegistry.h"
#include <entt/entt.hpp>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace simcore {

class FluidClient;
class PipeEnergyClient;

class BoilerSystem : public ISystem {
public:
  BoilerSystem(entt::registry &reg, std::shared_ptr<IEventPublisher> events,
               std::shared_ptr<PipeEnergyClient> pipeClient,
               std::shared_ptr<FluidClient> fluidClient = nullptr);

  void tick(float dt) override;

private:
  entt::registry &reg_;
  std::shared_ptr<IEventPublisher> events_;
  std::shared_ptr<PipeEnergyClient> pipeClient_;
  std::shared_ptr<FluidClient> fluidClient_;

  // Solid-fuel boiler (1110:01:0) per-entity burn state:
  // remaining burn energy (steam units) of the currently burning fuel item.
  std::unordered_map<entt::entity, int32_t> burnEnergy_;
};

} // namespace simcore
