#pragma once

#include "../../Network/IEventPublisher.h"
#include "../components/EnergyStorage.h"
#include "../components/InventoryContainer.h"
#include "../components/MachineComponent.h"
#include "ISystem.h"
#include "MachineRegistry.h"
#include "Network/PipeEnergyClient.h"
#include <entt/entt.hpp>
#include <memory>
#include <unordered_map>

namespace simcore {

class PipeEnergyClient;

class GeneratorSystem : public ISystem {
public:
  GeneratorSystem(entt::registry &reg, std::shared_ptr<IEventPublisher> events,
                  std::shared_ptr<PipeEnergyClient> pipeClient);

  void tick(float dt) override;

  static const std::unordered_map<uint16_t, int32_t> &FuelValues();

private:
  entt::registry &reg_;
  std::shared_ptr<IEventPublisher> events_;
  std::shared_ptr<PipeEnergyClient> pipeClient_;
  std::unordered_map<entt::entity, int32_t> burnEnergy_;
  std::unordered_map<entt::entity, uint16_t> burnFuel_;
};

} // namespace simcore
