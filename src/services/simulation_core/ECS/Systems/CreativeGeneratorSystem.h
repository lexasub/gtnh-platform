#pragma once

#include "../../Network/IEventPublisher.h"
#include "../components/EnergyStorage.h"
#include "../components/MachineComponent.h"
#include "ISystem.h"
#include "MachineRegistry.h"
#include "Network/PipeEnergyClient.h"
#include <common/ItemId.h>
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
      ItemId::pack("1110:01:2");

  void setEnergyPerTick(int32_t val) { energyPerTick_ = val; }

private:
  entt::registry &reg_;
  std::shared_ptr<IEventPublisher> events_;
  std::shared_ptr<PipeEnergyClient> pipeClient_;
  int32_t energyPerTick_ = kDefaultEnergyPerTick;
};

} // namespace simcore
