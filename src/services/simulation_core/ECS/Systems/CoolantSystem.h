#pragma once

#include "ISystem.h"
#include "HeatConstants.h"
#include "../components/HeatIntakeComponent.h"
#include "../components/OverheatComponent.h"
#include "../components/InventoryContainer.h"
#include "../components/Position.h"
#include "../components/MultiblockController.h"
#include "../components/EnergyStorage.h"
#include <entt/entt.hpp>
#include <spdlog/spdlog.h>

namespace simcore {

class CoolantSystem : public ISystem {
public:
  explicit CoolantSystem(entt::registry &reg) : reg_(reg) {}

  void tick(float /*dt*/) override {
    auto view = reg_.view<HeatIntakeComponent, OverheatComponent,
                          InventoryContainer, Position, MultiblockController>();

    for (auto ent : view) {
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
