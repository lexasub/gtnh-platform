# Task A5: Mining Speed Formula + CAS Energy Consumption

## Objective
Implement mining speed based on tool tier vs block hardness, and consume energy from the tool on each successful block break via CAS (Compare-And-Swap).

## Requirements

### 5.1 Mining speed formula
**Location**: `src/services/simulation_core/Tools/MiningCalculator.h` (NEW)

```cpp
#pragma once
#include <cstdint>
#include <cmath>

// Base mining time in ticks for a player with no tool
constexpr float BASE_MINE_TIME = 20.0f;  // 1 second at 20 Hz

// Tool speed multipliers per tier
constexpr float TOOL_SPEED[] = {
    1.5f,   // tier 0 (ULV) — 1.5x hand speed
    3.0f,   // tier 1 (LV)  — 3x hand speed
    5.0f,   // tier 2 (MV)  — 5x hand speed
    8.0f,   // tier 3 (HV)  — 8x hand speed
};

// Energy cost per block hardness unit
constexpr int32_t ENERGY_PER_HARDNESS = 50;  // 50 EU per hardness level

// Block hardness definitions (hardcoded for MVP, configurable later)
constexpr uint8_t getBlockHardness(uint16_t block_id) {
    switch (block_id) {
        // Ores
        case 3:  case 5:  case 25: case 53:  // iron, gold, tin, copper
        case 70: case 71: case 72: case 73:  // coal, redstone, lapis, diamond
            return 3;
        // Stone variants
        case 1:  return 2;  // stone
        case 7:  return 2;  // cobblestone
        case 8:  return 2;  // stone (smooth)
        // Soft blocks
        case 13: return 1;  // oak_planks
        case 12: return 0;  // glass
        default:  return 1;  // default
    }
}

// Mining level required to break a block
constexpr uint8_t getBlockMiningLevel(uint16_t block_id) {
    switch (block_id) {
        case 73: return 3;  // diamond_ore — needs HV (tier 3)
        case 72: return 2;  // lapis_ore — needs MV (tier 2)
        case 71: return 2;  // redstone_ore — needs MV
        case 5:  return 2;  // gold_ore — needs MV
        default: return 1;  // most ores — needs LV (tier 1)
    }
}

// Mining cost in EU
inline int32_t miningEnergyCost(uint16_t toolItem, uint16_t blockId) {
    return getBlockHardness(blockId) * ENERGY_PER_HARDNESS;
}

// Mining speed in ticks
inline float miningTicks(uint8_t toolTier, uint16_t blockId) {
    float speed = (toolTier < 4) ? TOOL_SPEED[toolTier] : TOOL_SPEED[3];
    return BASE_MINE_TIME / speed;
}
```

### 5.2 CAS energy consumption in ActionDispatcher
**Location**: `src/services/simulation_core/main.cpp` (extending Task A3)

```cpp
void ActionDispatcher::handleMineBlock(const Protocol::ToolAction* action) {
    auto playerInv = inventoryStore->getInventory(action->player_id());
    auto* toolSlot = playerInv->getSlot(action->slot_idx());
    
    if (!toolSlot || toolSlot->item_id != action->item_id()) {
        sendToolError(action, "no_tool");
        return;
    }
    
    // Check mining level
    uint8_t blockMinLevel = getBlockMiningLevel(action->pos()->block_id());  // need block_id of target
    uint8_t toolTier = ::toolTier(action->item_id());
    if (toolTier < blockMinLevel) {
        sendToolError(action, "wrong_tier");
        return;
    }
    
    // Check energy
    int32_t energy = getToolEnergy(*toolSlot);
    int32_t cost = miningEnergyCost(action->item_id(), action->pos()->block_id());
    if (energy < cost) {
        sendToolError(action, "no_energy");
        return;
    }
    
    // Execute CAS: set block to AIR
    auto casResult = casHandler(action->pos()->x(), action->pos()->y(), action->pos()->z(),
                                action->pos()->block_id(), 0);  // 0 = AIR
    
    if (casResult.success) {
        // Deduct energy
        setToolEnergy(*toolSlot, energy - cost);
        // Drop item to player inventory
        inventoryStore->giveItem(action->player_id(), action->pos()->block_id(), 1);
        // Send response
        sendToolResponse(action, true, action->pos()->block_id(), getToolEnergy(*toolSlot));
    } else {
        sendToolError(action, "cas_failed");
    }
}
```

### 5.3 Tool break animation timing
Client should play break animation for `miningTicks(tier, blockId)` before sending MINE_BLOCK. The server also validates: if tool breaks instantly (very fast), server accepts immediately.

**Tool break time examples:**

| Block | Hand | ULV Drill | LV Drill | MV Drill | HV Drill |
|-------|------|-----------|----------|----------|----------|
| Stone (hardness=2) | 20t (1s) | 13t | 7t | 4t | 3t |
| Iron Ore (hardness=3) | 20t (1s) | 13t | 7t | 4t | 3t |
| Diamond (hardness=3, min LV) | — | 13t | 7t | 4t | 3t |

## Acceptance Criteria
- [ ] `MiningCalculator.h` with speed, cost, hardness, mining level functions
- [ ] `getBlockHardness()` returns correct values per block
- [ ] `getBlockMiningLevel()` requires LV+ for most ores, HV for diamond
- [ ] `miningEnergyCost()` = hardness * 50 EU
- [ ] MINE_BLOCK handler checks tier ≥ mining level
- [ ] MINE_BLOCK handler checks energy ≥ cost
- [ ] CAS successful → energy deducted, item dropped
- [ ] CAS fails → energy NOT deducted, error response
- [ ] Wrong tier → "wrong_tier" error

## Dependencies
- Task A1 (tool item IDs + toolTier)
- Task A3 (ActionDispatcher — MINE_BLOCK handler)
- Task A4 (ItemEnergyStorage — getToolEnergy/setToolEnergy)

## Files to Create/Modify
- `src/services/simulation_core/Tools/MiningCalculator.h` — NEW
- `src/services/simulation_core/main.cpp` — extend handleMineBlock
