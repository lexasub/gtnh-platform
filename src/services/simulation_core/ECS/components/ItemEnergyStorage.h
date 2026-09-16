// ItemEnergyStorage.h
#pragma once
#include "../../InventoryActionHandler.h"
#include "../../../../common/ItemId.h"
#include "../../../../data/registry/ToolIds.h"
#include <algorithm>
#include <cstdint>
#include <unordered_map>

struct ToolEnergyDef {
  uint16_t itemId;
  int32_t capacity; // Max EU storage
  int32_t maxInput; // Max charge rate (EU/tick)
  uint8_t tier;     // Tool tier
};

// Tool and rechargeable battery energy definitions. Battery cells use the
// same metadata-backed energy contract as powered tools.
inline const std::unordered_map<uint16_t, ToolEnergyDef> TOOL_ENERGY_DEFS = {
    {90, {90, 1000, 8, 0}},
    {91, {91, 4000, 32, 1}},
    {92, {92, 16000, 128, 2}},
    {93, {93, 64000, 512, 3}},
    {94, {94, 4000, 32, 1}},
    {60948, {60948, 1000, 32, 0}},
    {60949, {60949, 4000, 128, 1}},
    {60950, {60950, 16000, 512, 2}},
};

inline constexpr uint16_t BATTERY_LV = 60948;
inline constexpr uint16_t BATTERY_MV = 60949;
inline constexpr uint16_t BATTERY_HV = 60950;

inline bool isRechargeableBattery(uint16_t item_id) {
  return item_id == BATTERY_LV || item_id == BATTERY_MV || item_id == BATTERY_HV;
}

inline int32_t getToolEnergy(const simulation_core::ItemStack& item) {
  auto it = TOOL_ENERGY_DEFS.find(item.item_id);
  if (it == TOOL_ENERGY_DEFS.end()) {
    return -1;
  }
  return static_cast<int32_t>(item.meta);
}

inline void setToolEnergy(simulation_core::ItemStack& item, int32_t energy) {
  auto it = TOOL_ENERGY_DEFS.find(item.item_id);
  if (it == TOOL_ENERGY_DEFS.end()) {
    return;
  }
  item.meta = static_cast<uint16_t>(std::clamp(energy, 0, it->second.capacity));
}

inline bool consumeToolEnergy(simulation_core::ItemStack& item, int32_t amount) {
  int32_t current = getToolEnergy(item);
  if (current < amount) {
    return false;
  }
  setToolEnergy(item, current - amount);
  return true;
}
