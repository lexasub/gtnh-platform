// ItemEnergyStorage.h
#pragma once
#include <apps/simcore/InventoryActionHandler.h>
#include <engine/registry/ItemId.h>
#include <data/registry/ToolIds.h>
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
    // The four shipped drills (gp-v4re). These keys used to be the bare numbers
    // 90-94, which match NO item in items.csv - grep -cE "^(90|91|92|93|94)([:,]|$)"
    // returns 0 for every one of them - so DrillSystem::findToolSlot, which
    // looks a tool up by item_id, could never match a real drill and the whole
    // drill energy path was unreachable. The ids come from ToolIds.h, the repo's
    // own compile-time pack of the registry notation; the capacities and charge
    // rates are unchanged from the rows that were already here, so no new number
    // is invented by this change - the four entries are simply attached to the
    // tools they were always describing, in the same ULV/LV/MV/HV order.
    {ITEM_DRILL_ULV, {ITEM_DRILL_ULV, 1000, 8, 0}},
    {ITEM_DRILL_LV, {ITEM_DRILL_LV, 4000, 32, 1}},
    {ITEM_DRILL_MV, {ITEM_DRILL_MV, 16000, 128, 2}},
    {ITEM_DRILL_HV, {ITEM_DRILL_HV, 64000, 512, 3}},

    // Battery cells. These three were already correct: items.csv carries
    // battery_lv/mv/hv as 1110:111:20/21/22, which pack to exactly 60948/60949/60950.
    // (the BATTERY_* constants are declared just below this table, so the
    // packed forms are spelled out here to keep the declaration order simple)
    {60948, {60948, 1000, 32, 0}},  // == BATTERY_LV, pack("1110:111:20")
    {60949, {60949, 4000, 128, 1}},  // == BATTERY_MV, pack("1110:111:21")
    {60950, {60950, 16000, 512, 2}}, // == BATTERY_HV, pack("1110:111:22")
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
