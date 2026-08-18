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
class FluidClient;

class GeneratorSystem : public ISystem {
public:
  GeneratorSystem(entt::registry &reg, std::shared_ptr<IEventPublisher> events,
                  std::shared_ptr<PipeEnergyClient> pipeClient,
                  std::shared_ptr<FluidClient> fluidClient = nullptr);

  void tick(float dt) override;

  static const std::unordered_map<uint16_t, int32_t> &FuelValues();

private:
  entt::registry &reg_;
  std::shared_ptr<IEventPublisher> events_;
  std::shared_ptr<PipeEnergyClient> pipeClient_;
  std::shared_ptr<FluidClient> fluidClient_;
  std::unordered_map<entt::entity, int32_t> burnEnergy_;
  std::unordered_map<entt::entity, uint16_t> burnFuel_;

  // Throttle the always-on STEAM boiler registration. An idle boiler only needs
  // to register once so adjacent pipes can attach; re-publish on a low-frequency
  // heartbeat for late pipe placement instead of every tick.
  uint64_t tickCount_ = 0;
  std::unordered_map<entt::entity, uint64_t> lastSteamPublish_;
  static constexpr uint64_t kSteamHeartbeatTicks = 40;  // ~2 s at 20 Hz
};

} // namespace simcore
