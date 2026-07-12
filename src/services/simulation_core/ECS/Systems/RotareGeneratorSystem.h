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

struct RotareState {
  bool spinning = false;
  int32_t remainingTicks = 0;
  int32_t energyPerTick = 32;
};

class RotareGeneratorSystem : public ISystem {
public:
  RotareGeneratorSystem(entt::registry &reg,
                        std::shared_ptr<IEventPublisher> events,
                        std::shared_ptr<PipeEnergyClient> pipeClient);

  void tick(float dt) override;
  void activate(entt::entity ent);

  static constexpr uint16_t kRotareGeneratorBlockId =
      ItemId::pack("1110:01:3");
  static constexpr int32_t kSpinDurationTicks = 100;
  static constexpr int32_t kEnergyPerTick = 32;

private:
  entt::registry &reg_;
  std::shared_ptr<IEventPublisher> events_;
  std::shared_ptr<PipeEnergyClient> pipeClient_;
};

} // namespace simcore
