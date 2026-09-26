#pragma once

#include <engine/sim/ISystem.h>
#include "HeatConstants.h"
#include <engine/sim/components/HeatIntakeComponent.h>
#include <game/machines/OverheatComponent.h>
#include <engine/sim/components/InventoryContainer.h>
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/components/Position.h>
#include <engine/sim/components/EnergyStorage.h>
#include <entt/entt.hpp>
#include <spdlog/spdlog.h>

namespace simcore {

class CoolantSystem : public ISystem {
public:
  explicit CoolantSystem(entt::registry &reg) : reg_(reg) {}

  void tick(float /*dt*/) override {
    // Candidate gate: HeatIntakeComponent + OverheatComponent +
    // InventoryContainer + Position, and the entity must be a live multiblock
    // controller (MachineComponent::mb_id != 0).
    //
    // This used to be a 5-type view keyed on a `MultiblockController` ECS
    // component (gp-qgtc). That component is never emplaced in production:
    // SimulationEngine owns controllers in a plain
    // `std::unordered_map<uint64_t, MultiblockController> controllers_`
    // (SimulationEngine.h:108), mutated in place by EBFSystem / LCRSystem /
    // LargeBoilerSystem, so an ECS mirror would be a second source of truth
    // free to desync. `mb_id` is the field the engine already maintains:
    // set on formation (SimulationEngine.cpp:309), updated on every block
    // echo (:370), and removed with the MachineComponent on teardown
    // (destroyController, :82). Only the anchor is a machine — member blocks
    // are casing/coil — so `mb_id != 0` is exactly "controller anchor".
    auto view = reg_.view<HeatIntakeComponent, OverheatComponent,
                          InventoryContainer, Position>();

    for (auto ent : view) {
      if (const auto *machine = reg_.try_get<MachineComponent>(ent)) {
        if (machine->mb_id == 0) continue;
      } else {
        continue;
      }
      auto &hic = view.get<HeatIntakeComponent>(ent);
      auto &oh = view.get<OverheatComponent>(ent);
      auto &inventory = view.get<InventoryContainer>(ent);
      auto &pos = view.get<Position>(ent);

      if (oh.state != OverheatState::WARNING && oh.state != OverheatState::CRITICAL) continue;
      if (hic.heat_stored <= 0) continue;

      bool has_coolant = false;
      for (const auto &slot : inventory.slots) {
        if (slot.item_id == HeatConstants::COOLANT_ITEM_ID) {
          has_coolant = true;
          break;
        }
      }
      if (!has_coolant) continue;

      for (auto &slot : inventory.slots) {
        if (slot.item_id == HeatConstants::COOLANT_ITEM_ID) {
          if (slot.count > 0) {
            slot.count--;
            if (slot.count == 0) slot.item_id = 0;
            break;
          }
        }
      }

      int32_t cool_amount = static_cast<int32_t>(HeatConstants::COOLANT_COOLING_AMOUNT);
      if (cool_amount > hic.heat_stored) cool_amount = hic.heat_stored;
      hic.heat_stored -= cool_amount;

      if (auto *energy = reg_.try_get<EnergyStorage>(ent)) {
        if (energy->type == EnergyType::HEAT) {
          energy->current = hic.heat_stored;
        }
      }

      spdlog::debug("[Coolant] Cooled machine {} at ({},{},{}) - reduced {} heat",
                     static_cast<uint32_t>(ent), pos.x, pos.y, pos.z, cool_amount);
    }
  }

private:
  entt::registry &reg_;
};

} // namespace simcore
