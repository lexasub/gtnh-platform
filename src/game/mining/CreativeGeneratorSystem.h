#pragma once

#include "apps/simcore/Network/IEventPublisher.h"
#include <engine/sim/components/EnergyStorage.h>
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/ISystem.h>
#include <game/machines/MachineRegistry.h>
#include "apps/simcore/Network/PipeEnergyClient.h"
#include <engine/registry/ItemId.h>
#include <entt/entt.hpp>
#include <memory>

namespace simcore {

class PipeEnergyClient;

class CreativeGeneratorSystem : public ISystem {
public:
  CreativeGeneratorSystem(entt::registry &reg,
                          std::shared_ptr<IEventPublisher> events,
                          std::shared_ptr<PipeEnergyClient> pipeClient);

  void tick(float dt) override;

  static constexpr int32_t kDefaultEnergyPerTick = 1024;
  static constexpr uint16_t kCreativeGeneratorBlockId =
      ItemId::pack("1110:100:0");

  void setEnergyPerTick(int32_t val) { energyPerTick_ = val; }

private:
  entt::registry &reg_;
  std::shared_ptr<IEventPublisher> events_;
  std::shared_ptr<PipeEnergyClient> pipeClient_;
  int32_t energyPerTick_ = kDefaultEnergyPerTick;
};

} // namespace simcore
