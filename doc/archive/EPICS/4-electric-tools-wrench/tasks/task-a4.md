# Task A4: EnergyStorage in Tool Items

## Objective
Add energy storage capability to tool items so drills/chainsaws can hold and consume EU. Tools use a per-item EnergyStorage (not ECS component — items aren't entities).

## Requirements

### 4.1 Item energy storage design
**Location**: `src/services/simulation_core/Inventory/ItemEnergyStorage.h` (NEW)

Unlike machines (which have ECS `EnergyStorage` component), tools are items in inventory. Energy must be stored in the item's metadata.

```cpp
#pragma once
#include <cstdint>
#include <unordered_map>

struct ToolEnergyDef {
    uint16_t itemId;
    int32_t capacity;       // Max EU storage
    int32_t maxInput;       // Max charge rate (EU/tick)
    uint8_t tier;           // Tool tier
};

// Tool energy definitions
const std::unordered_map<uint16_t, ToolEnergyDef> TOOL_ENERGY_DEFS = {
    // itemId   {itemId, capacity, maxInput, tier}
    {90,  {90,  1000,   8,   0}},  // drill_ulv:  1000 EU
    {91,  {91,  4000,  32,   1}},  // drill_lv:   4000 EU
    {92,  {92,  16000, 128,  2}},  // drill_mv:   16000 EU
    {93,  {93,  64000, 512,  3}},  // drill_hv:   64000 EU
    {94,  {94,  4000,  32,   1}},  // chainsaw_lv: 4000 EU
};
```

### 4.2 Energy storage in item meta
**Location**: `src/services/simulation_core/Inventory/` — item representation

Since items are stored as `ItemStack(id, count, meta)`:
```
meta = current_energy  // stored as uint16_t in item stack meta field
```

```cpp
// Get/set tool energy
inline int32_t getToolEnergy(const ItemStack& item) {
    auto it = TOOL_ENERGY_DEFS.find(item.item_id);
    if (it == TOOL_ENERGY_DEFS.end()) return -1;
    return static_cast<int32_t>(item.meta);  // meta = current energy
}

inline void setToolEnergy(ItemStack& item, int32_t energy) {
    auto it = TOOL_ENERGY_DEFS.find(item.item_id);
    if (it == TOOL_ENERGY_DEFS.end()) return;
    item.meta = static_cast<uint16_t>(std::clamp(energy, 0, it->second.capacity));
}

inline bool consumeToolEnergy(ItemStack& item, int32_t amount) {
    int32_t current = getToolEnergy(item);
    if (current < amount) return false;
    setToolEnergy(item, current - amount);
    return true;
}
```

### 4.3 Tool charge state
- Full charge: `meta = capacity`
- Empty: `meta = 0`
- Tooltip shows: "Energy: 4000/4000 EU" (client reads meta)

### 4.4 Limitations
- uint16_t meta = max 65535 EU — OK for LV/MV, HV tools need >65535
  - For HV+ tools: use effective_energy = meta * 4 (60000→240000)
  - Or switch to multiple meta fields (requires protocol change)
  - **Recommendation**: keep uint16_t for now, use multiplier for HV+

## Acceptance Criteria
- [ ] `TOOL_ENERGY_DEFS` with all 5 tools
- [ ] `getToolEnergy(item)` returns current energy from meta
- [ ] `setToolEnergy(item, energy)` updates meta with clamping
- [ ] `consumeToolEnergy(item, amount)` returns false if insufficient
- [ ] Tool energy persists in inventory (meta field saved/loaded)
- [ ] Default energy for new tool = max (full charge) or 0 (uninterpreted)

## Dependencies
- Task A1 (tool item IDs)
- Required by: Task A6 (mining cost), Task A7 (battery charging)

## Files to Create/Modify
- `src/services/simulation_core/Inventory/ItemEnergyStorage.h` — NEW
