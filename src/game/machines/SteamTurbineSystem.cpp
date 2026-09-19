#include "SteamTurbineSystem.h"

#include <algorithm>
#include <cstdint>

namespace simcore {

SteamTurbineSystem::SteamTurbineSystem(
    entt::registry& reg, std::shared_ptr<IEventPublisher> events,
    std::shared_ptr<PipeEnergyClient> energyClient,
    std::shared_ptr<FluidClient> fluidClient, std::uint16_t steam_item_id)
    : reg_(reg), events_(std::move(events)),
      energyClient_(std::move(energyClient)), fluidClient_(std::move(fluidClient)),
      steam_item_id_(steam_item_id) {}

void SteamTurbineSystem::tick(float /*dt*/) {
  auto view = reg_.view<MachineComponent, SteamTurbineComponent>();
  for (auto entity : view) {
    auto& machine = view.get<MachineComponent>(entity);
    auto& turbine = view.get<SteamTurbineComponent>(entity);
    if (machine.machine_id != kBlockId) continue;

    if (turbine.steam_item_id == 0) turbine.steam_item_id = steam_item_id_;
    const auto x = static_cast<std::int32_t>(machine.x);
    const auto y = static_cast<std::int32_t>(machine.y);
    const auto z = static_cast<std::int32_t>(machine.z);

    if (fluidClient_ && turbine.steam_item_id != 0) {
      fluidClient_->publishNodeUpdate(
          static_cast<std::uint64_t>(entity), x, y, z, turbine.steam_item_id,
          turbine.steam_stored, turbine.steam_capacity, 0,
          turbine.steam_max_input, 0, false, true);
      if (turbine.steam_stored < turbine.steam_capacity &&
          !turbine.request_pending) {
        const auto amount = std::min(turbine.steam_max_input,
                                     turbine.steam_capacity - turbine.steam_stored);
        if (amount > 0) {
          fluidClient_->sendFluidRequest(static_cast<std::uint64_t>(entity), x, y,
                                         z, turbine.steam_item_id, amount);
          turbine.request_pending = true;
          pending_entity_ = entity;
        }
      }
    }

    const auto eu_space = turbine.eu_capacity - turbine.eu_stored;
    const auto steam_to_use = std::min({turbine.steam_stored,
                                        turbine.eu_max_output,
                                        eu_space / std::max(1, turbine.eu_per_steam)});
    if (steam_to_use > 0) {
      turbine.steam_stored -= steam_to_use;
      turbine.eu_stored += steam_to_use * turbine.eu_per_steam;
    }

    if (energyClient_) {
      energyClient_->publishNodeUpdate(
          static_cast<std::uint64_t>(entity), x, y, z, turbine.eu_stored,
          turbine.eu_capacity, 0, turbine.eu_max_output, 0,
          static_cast<std::int32_t>(EnergyType::ELECTRICITY), true, false);
    }
    if (events_) {
      events_->publishBlockEntityUpdate(
          machine.x, machine.y, machine.z, machine.machine_id, {}, 1.0f,
          static_cast<std::uint32_t>(turbine.eu_stored), EnergyType::ELECTRICITY,
          static_cast<std::uint32_t>(turbine.eu_capacity), 0, 0.0f, nullptr,
          static_cast<double>(turbine.steam_stored),
          static_cast<double>(turbine.steam_capacity));
    }
  }
}

void SteamTurbineSystem::onFluidConsumeResponse(std::int32_t consumed) {
  if (pending_entity_ == entt::null) return;
  const auto entity = pending_entity_;
  pending_entity_ = entt::null;
  if (!reg_.valid(entity)) return;
  auto* turbine = reg_.try_get<SteamTurbineComponent>(entity);
  if (!turbine) return;
  turbine->request_pending = false;
  if (consumed > 0) {
    turbine->steam_stored = std::min(turbine->steam_capacity,
                                      turbine->steam_stored + consumed);
  }
}


} // namespace simcore
