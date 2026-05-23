# Task 8: Fluid Item Registries (water, steam, sulfuric_acid)

## Objective
Add fluid item IDs to the item registry so fluids can be transported as items (architectural decision: fluids-as-items, no separate FluidTankComponent needed).

## Requirements

### 8.1 Define fluid item IDs
**Location**: `data/registry/items.csv`

Add fluid entries as regular items:
```
water, 1000, fluid
steam, 1001, fluid
sulfuric_acid, 1002, fluid
```

**Fields** (verify from items.csv format):
- `name` — unique item name
- `id` — numeric item ID (use high range 1000+ to avoid collision with blocks)
- `type` — "fluid" category (or whatever type column exists)

### 8.2 Add fluid constants to codebase
**Location**: `src/data/registry/ItemRegistry.h` or `src/protocol/core.fbs`

```cpp
// C++ constants
constexpr uint16_t ITEM_ID_WATER         = 1000;
constexpr uint16_t ITEM_ID_STEAM         = 1001;
constexpr uint16_t ITEM_ID_SULFURIC_ACID = 1002;
```

### 8.3 Add fluid definitions to MachineRegistry or FluidRegistry
**Location**: `src/services/pipe_network/` — new `FluidRegistry.h` or extend `MachineRegistry`

```cpp
struct FluidDef {
    uint16_t item_id;
    std::string name;
    float density;           // for gravity: >0 sinks, <0 rises
    float viscosity;         // flow speed multiplier (1.0 = water default)
    uint16_t max_temp;       // max temperature before vaporizing
};

class FluidRegistry {
public:
    void registerFluid(const FluidDef& def);
    const FluidDef* getFluid(uint16_t item_id) const;
    bool isFluid(uint16_t item_id) const;
    
    static FluidRegistry& instance();
    
private:
    std::unordered_map<uint16_t, FluidDef> m_fluids;
};
```

### 8.4 Integration with existing item system
- MachineRecipe system should accept fluid item_id the same as normal items
- `ConditionEvaluator::getMachineState()` should include fluid item IDs
- Fluid items stack like normal items (stackable)

### 8.5 Fluid display name
**Location**: `src/services/game_client/` — item tooltip rendering

Fluid items show:
- Name: "Water" (not "Water Bucket" — fluids are abstract items)
- Amount: displayed as "1000mB" or similar (1 item = 1000mB)

## Future Use
These fluid IDs will be used by:
- BoilerSystem (produces steam item)
- Steam machines (consumes steam item)
- PipeNetwork fluid graph (transports fluid items)
- RecipeManager (fluid + solid recipes for LCR)

## Acceptance Criteria
- [ ] Water, steam, sulfuric_acid entries in items.csv
- [ ] Item ID constants in codebase
- [ ] `FluidRegistry` with lookup by item_id
- [ ] `isFluid(item_id)` correctly identifies fluid items
- [ ] Fluid items appear in-game (basic item, no special behavior yet)
- [ ] No compilation errors

## Dependencies
- None — registry changes only
- Required by: Task 9 (fluid graph), Task 10 (boiler→pipe), Task 13 (RecipeManager)

## Files to Modify
- `data/registry/items.csv` — fluid entries
- `src/services/pipe_network/` — new `FluidRegistry.h/.cpp`
- Possibly `src/services/simulation_core/` — include fluid constants
