#pragma once

#include <engine/sim/ISystem.h>
#include <game/machines/SteamTurbineComponent.h>
#include <engine/sim/components/MachineComponent.h>
#include "Network/IEventPublisher.h"
#include "Network/PipeEnergyClient.h"
#include "Network/FluidClient.h"

#include <engine/registry/ItemId.h>
#include <entt/entt.hpp>
#include <memory>

namespace simcore {

class SteamTurbineSystem final : public ISystem {
public:
  SteamTurbineSystem(entt::registry& reg,
                     std::shared_ptr<IEventPublisher> events,
                     std::shared_ptr<PipeEnergyClient> energyClient,
                     std::shared_ptr<FluidClient> fluidClient,
                     std::uint16_t steam_item_id);

  void tick(float dt) override;
  void onFluidConsumeResponse(std::int32_t consumed);

  static constexpr std::uint16_t kBlockId = ItemId::pack("1110:010:44");

private:
  entt::registry& reg_;
  std::shared_ptr<IEventPublisher> events_;
  std::shared_ptr<PipeEnergyClient> energyClient_;
  std::shared_ptr<FluidClient> fluidClient_;
  std::uint16_t steam_item_id_ = 0;
  entt::entity pending_entity_ = entt::null;
};

} // namespace simcore
