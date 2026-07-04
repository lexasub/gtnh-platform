// ItemEnergyStorage.h
#pragma once
#include "../../InventoryActionHandler.h"
#include "../../data/registry/ToolIds.h"
#include <algorithm>
#include <cstdint>
#include <unordered_map>

struct ToolEnergyDef {
  uint16_t itemId;
  int32_t capacity; // Max EU storage
  int32_t maxInput; // Max charge rate (EU/tick)
  uint8_t tier;     // Tool tier
};

// Tool energy definitions
inline const std::unordered_map<uint16_t, ToolEnergyDef> TOOL_ENERGY_DEFS = {
    // itemId   {itemId, capacity, maxInput, tier}
    {90, {90, 1000, 8, 0}},    // drill_ulv:  1000 EU
    {91, {91, 4000, 32, 1}},   // drill_lv:   4000 EU
    {92, {92, 16000, 128, 2}}, // drill_mv:   16000 EU
    {93, {93, 64000, 512, 3}}, // drill_hv:   64000 EU
    {94, {94, 4000, 32, 1}},   // chainsaw_lv: 4000 EU
};

// Get/set tool energy
inline int32_t getToolEnergy(const simulation_core::ItemStack &item) {
  auto it = TOOL_ENERGY_DEFS.find(item.item_id);
  if (it == TOOL_ENERGY_DEFS.end())
    return -1;
  return static_cast<int32_t>(item.meta); // meta = current energy
}

inline void setToolEnergy(simulation_core::ItemStack &item, int32_t energy) {
  auto it = TOOL_ENERGY_DEFS.find(item.item_id);
  if (it == TOOL_ENERGY_DEFS.end())
    return;
  item.meta = static_cast<uint16_t>(std::clamp(energy, 0, it->second.capacity));
}

inline bool consumeToolEnergy(simulation_core::ItemStack &item,
                              int32_t amount) {
  int32_t current = getToolEnergy(item);
  if (current < amount)
    return false;
  setToolEnergy(item, current - amount);
  return true;
}