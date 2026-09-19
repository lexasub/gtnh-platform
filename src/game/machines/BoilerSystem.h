#pragma once

#include "Network/IEventPublisher.h"
#include "Network/PipeEnergyClient.h"
#include <engine/sim/components/EnergyStorage.h>
#include <game/machines/HeatIntakeComponent.h>
#include <engine/sim/components/InventoryContainer.h>
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/components/RecipeProgress.h>
#include "BoilerPorts.h"
#include "Network/ResourceBufferStatePublisher.h"
#include <engine/sim/ISystem.h>
#include "MachineRegistry.h"
#include <entt/entt.hpp>
#include <memory>

namespace simcore {

class FluidClient;
class PipeEnergyClient;

}  // namespace simcore

namespace gtnh::common {
class IResourcePortClient;
}

namespace simcore {

class BoilerSystem : public ISystem {
public:
  BoilerSystem(entt::registry &reg, std::shared_ptr<IEventPublisher> events,
               std::shared_ptr<PipeEnergyClient> pipeClient,
               std::shared_ptr<FluidClient> fluidClient = nullptr,
               std::shared_ptr<gtnh::common::IResourcePortClient> portClient = nullptr,
               std::uint16_t steam_item_id = 0,
               std::shared_ptr<ResourceBufferStatePublisher> statePublisher = nullptr);

  void tick(float dt) override;

  // Announce a replacement of one owned port: bumps its epoch (monotonic per
  // port); the next tick republishes that port with the new epoch. Returns
  // the new epoch.
  std::uint64_t replacePort(std::uint64_t owner_id, gtnh::common::PortId port_id);

private:
  entt::registry &reg_;
  std::shared_ptr<IEventPublisher> events_;
  std::shared_ptr<PipeEnergyClient> pipeClient_;
  std::shared_ptr<FluidClient> fluidClient_;
  // Typed resource-port publisher (openspec refactor-fluid-port-accounting
  // 2.4): HU sink + FLUID steam source, published alongside the legacy node
  // updates until the typed path is end-to-end.
  std::shared_ptr<gtnh::common::IResourcePortClient> portClient_;
  std::shared_ptr<ResourceBufferStatePublisher> statePublisher_;
  BoilerPorts::PortEpochBook port_epochs_;
  // Registry-resolved Steam item id; 0 (registry unavailable) fails closed:
  // no conversion, no steam advertisement.
  std::uint16_t steam_id_ = 0;
};

} // namespace simcore
