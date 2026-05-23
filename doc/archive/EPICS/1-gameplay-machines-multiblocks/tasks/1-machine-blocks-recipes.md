# TASK: Simple Machine Blocks & Recipes
**Layer**: 1
**Status**: Draft
**Epic**: 1-gameplay-machines-multiblocks

## Affected Services

| Service | Role | R/W |
|---------|------|-----|
| **SimulationCore** | Primary — owns machine ECS components and block registration | Read/Write |
| **RecipeManager** ⬅️ NEW | Dependency — stores machine-specific recipes | Read |
| **ChunkStore** | Block storage — stores machine block IDs in chunk data | Write |
| **GameClient** | Consumer — renders machine blocks, needs block ID registry | Read |



## Overview

This specification defines the simple machine blocks (FURNACE, MACERATOR, COMPRESSOR) and their associated recipe system for the GTNH Platform. These machines operate as individual processing units that accept inputs, consume energy, and produce outputs over defined durations. They do not form multiblock structures and are intended for early-game accessibility.

## Block IDs

| ID | Block Name | Category |
|----|------------|----------|
| `101` | FURNACE | Machine |
| `102` | MACERATOR | Machine |
| `103` | COMPRESSOR | Machine |

### Block Properties

```cpp
struct MachineBlock : Block {
    enum Type {
        FURNACE   = 101,
        MACERATOR = 102,
        COMPRESSOR = 103,
    };
    Type type;
    uint8_t  meta;  // 0 = idle, 1 = running, 2 = error
};
```

## Recipe Definitions

Three core recipe types define machine behavior:

| Recipe ID | Name | Input | Output | Energy | Duration (ticks) |
|-----------|------|-------|--------|--------|------------------|
| `101` | Iron Ore Smelt | Iron Ore | Iron Ingot | 100 | 120 |
| `102` | Copper Ore Smelt | Copper Ore | Copper Ingot | 100 | 120 |
| `103` | Gold Ore Smelt | Gold Ore | Gold Ingot | 100 | 120 |
| `104` | Iron Ore Macerate | Iron Ore | 2x Iron Dust | 50 | 60 |
| `105` | Copper Ore Macerate | Copper Ore | 2x Copper Dust | 50 | 60 |
| `106` | Gold Ore Macerate | Gold Ore | 2x Gold Dust | 50 | 60 |
| `107` | Iron Dust Compress | 2x Iron Dust | Iron Plate | 50 | 60 |
| `108` | Copper Dust Compress | 2x Copper Dust | Copper Plate | 50 | 60 |
| `109` | Gold Dust Compress | 2x Gold Dust | Gold Plate | 50 | 60 |

### Recipe Structure

```cpp
struct Processing::Recipe {
    uint16_t id;
    std::array<uint16_t, 2> input_ids;
    std::array<uint16_t, 1> output_ids;
    uint32_t energy_cost;
    uint32_t duration_ticks;
};
```

#### Notes
- Input slot 0 is required for all recipes
- Input slot 1 is required for COMPRESSOR recipes only
- Output slot 0 always produces a single stack (64 count)
- Energy is consumed per recipe execution

## RecipeManager Integration

The RecipeManager provides lookup and validation for machine operations:

### Methods

```cpp
class RecipeManager {
public:
    static RecipeManager& instance();
    
    bool canProcess(uint16_t recipe_id) const;
    Recipe const& getRecipe(uint16_t recipe_id) const;
    
    uint16_t findFurnaceRecipe(uint16_t ore_id);
    uint16_t findMaceratorRecipe(uint16_t ore_id);
    uint16_t findCompressorRecipe(uint16_t ore_id, uint16_t dust_id);
    
    bool isRecipeValid(uint16_t recipe_id) const;
    uint16_t getRequiredEnergy(uint16_t recipe_id) const;
    uint32_t getDurationTicks(uint16_t recipe_id) const;
};
```

### Integration Points

1. **Gateway** sends `PlayerAction::MACHINE_INSERT` / `MACHINE_EXTRACT`
2. **SimulationCore** queries RecipeManager to validate operations
3. **SimulationCore** updates machine state (running → idle)
4. **SimulationCore** emits `BlockChanged` when machine state transitions
5. **ChunkStore** persists machine state via `SetBlock`

## Data Structures

### Machine Inventory

```cpp
struct MachineInventory {
    struct Slot {
        uint16_t item_id;
        uint8_t  count;
    };
    Slot input_slots[2];
    Slot output_slots[1];
};
```

### Machine State

```cpp
struct MachineState {
    uint16_t recipe_id;      // 0 = none
    uint16_t energy_remaining;  // ticks until completion
    bool is_running;
};
```

### Machine Block Data

```cpp
struct MachineData : BlockData {
    uint16_t state_id;        // unique per machine instance
    uint8_t  progress;        // 0-100%
    MachineState state;
    MachineInventory inventory;
};
```

## File Locations

| File | Path |
|------|------|
| Block definitions | `src/services/simulation_core/include/Processing/BlockData.h` |
| Recipe data | `src/services/simulation_core/include/Processing/Recipe.h` |
| RecipeManager | `src/services/simulation_core/include/Processing/RecipeManager.h` |
| Machine state | `src/services/simulation_core/include/Processing/MachineState.h` |
| Protocol messages | `src/protocol/processing.fbs` |

## Acceptance Criteria

#### Scenario: Furnace smelts iron ore

WHEN a player places Iron Ore in the input slot of a FURNACE  
AND the FURNACE has sufficient energy  
AND a valid smelting recipe exists for Iron Ore  
THEN the FURNACE enters running state (meta = 1)  
AND after 120 ticks the output slot contains 1 Iron Ingot  
AND the input slot is cleared  
AND the FURNACE returns to idle state (meta = 0)

#### Scenario: Macerator processes ore into dust

WHEN a player places Iron Ore in the input slot of a MACERATOR  
AND the MACERATOR has sufficient energy  
AND a valid maceration recipe exists for Iron Ore  
THEN the MACERATOR enters running state (meta = 1)  
AND after 60 ticks the output slot contains 2 Iron Dust  
AND the input slot is cleared  
AND the MACERATOR returns to idle state (meta = 0)

#### Scenario: Compressor requires both input slots

WHEN a player places 2 Iron Dust in both input slots of a COMPRESSOR  
AND the COMPRESSOR has sufficient energy  
AND a valid compression recipe exists for the dust pair  
THEN the COMPRESSOR enters running state (meta = 1)  
AND after 60 ticks the output slot contains 1 Iron Plate  
AND both input slots are cleared  
AND the COMPRESSOR returns to idle state (meta = 0)

#### Scenario: Recipe lookup returns valid ID

WHEN RecipeManager::findFurnaceRecipe(6) is called for Iron Ore  
AND the recipe exists in the registry  
THEN findFurnaceRecipe returns 101  
AND RecipeManager::isRecipeValid(101) returns true

#### Scenario: Invalid recipe ID returns false

WHEN RecipeManager::canProcess(999) is called for a non-existent recipe  
AND the recipe does not exist in the registry  
THEN canProcess returns false  
AND isRecipeValid(999) returns false

#### Scenario: Energy cost is enforced

WHEN a machine attempts to process a recipe  
AND the machine has less energy than the recipe requires  
THEN the machine does not start processing  
AND the machine state remains unchanged  
AND RecipeManager::getRequiredEnergy returns the correct cost for each recipe
