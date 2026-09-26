#pragma once

#include "Network/IEventPublisher.h"
#include <engine/sim/components/EnergyStorage.h>
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/SimulationEngine.h>
#include <engine/sim/ISystem.h>
#include "MachineRegistry.h"
#include "Network/PipeEnergyClient.h"
#include <engine/registry/ItemId.h>
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

  // The registry this system ticks. The interaction wiring resolves the
  // clicked entity on it, so the entity it arms is the one tick() can see.
  entt::registry& registry() { return reg_; }

  static constexpr uint16_t kRotareGeneratorBlockId =
      ItemId::pack("1110:100:1");
  static constexpr int32_t kSpinDurationTicks = 100;
  static constexpr int32_t kEnergyPerTick = 32;

private:
  entt::registry &reg_;
  std::shared_ptr<IEventPublisher> events_;
  std::shared_ptr<PipeEnergyClient> pipeClient_;
};

// Wire the rotare generator into the engine's machine-interaction table, so a
// left-click actually spins it (gp-18yv).
//
// machines.yaml flags 1110:100:1 `interact_on_left: true`, and
// MachineInteractHandler turns that left-click into a call to
// SimulationEngine::onMachineInteracted — but the table that call consults was
// empty for every machine, so the click produced an ack, an animation, and no
// energy. RotareState is emplaced in exactly one place, activate(), so an
// unregistered system is dead at runtime no matter what tick() does.
//
// This is the registration that makes the system reachable. It belongs here,
// next to the system it activates, rather than in main.cpp, so a caller cannot
// construct the system and forget to wire it: the spin is started by the same
// call that installs the handler.
//
// Requires the system to outlive the engine registration, and both to outlive
// the entity's spin.
void registerRotareInteraction(SimulationEngine &engine,
                               RotareGeneratorSystem &system);

} // namespace simcore
