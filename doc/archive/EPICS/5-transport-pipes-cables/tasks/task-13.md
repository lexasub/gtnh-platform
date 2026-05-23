# Task 13: RecipeManager — Fluid Item Support

## Objective
Extend RecipeManager to handle recipes with fluid inputs and outputs (fluids-as-items). This enables machines like the Large Chemical Reactor (LCR) to process chemical recipes requiring both solids and fluids.

## Requirements

### 13.1 Extend recipe format for fluid items
**Location**: `data/recipes/` — recipe JSON files

Current recipe format (solid items only):
```json
{
    "machine": "chemical_reactor",
    "inputs": [{"item": "iron_ingot", "count": 3}],
    "outputs": [{"item": "iron_plate", "count": 1}],
    "eu_per_tick": 30,
    "total_tick": 100
}
```

Extended format with fluid support:
```json
{
    "machine": "chemical_reactor",
    "inputs": [
        {"item": "iron_ingot", "count": 3},
        {"fluid": "sulfuric_acid", "amount": 1000}
    ],
    "outputs": [
        {"item": "iron_sulfate", "count": 2},
        {"fluid": "hydrogen", "amount": 500, "byproduct": true}
    ],
    "eu_per_tick": 30,
    "total_tick": 100
}
```

**Parsing**:
- `"fluid"` key → lookup fluid item_id from registry
- `"amount"` → item count (1 item = 1000mB standard)
- `"byproduct"` → optional output (not required for recipe to complete)

### 13.2 Update RecipeParser
**Location**: `src/services/recipe_manager/RecipeParser.cpp` or `src/libs/recipe_manager/`

```cpp
struct RecipeIngredient {
    uint16_t itemId;
    uint8_t count;
    bool isFluid;         // NEW
};

struct RecipeResult {
    uint16_t itemId;
    uint8_t count;
    bool isFluid;         // NEW
    bool isByproduct;     // NEW
};

// Update Recipe to use these structs instead of flat item lists
```

### 13.3 Update ConditionEvaluator for fluid checks
**Location**: `src/services/simulation_core/Crafting/ConditionEvaluator.h/.cpp` or `MachineState`

```cpp
struct MachineState {
    // ... existing fields ...
    std::vector<ItemSlot> fluidInputs;  // NEW: fluid items in machine
};

// CheckFluidInputs() — verify machine has required fluid amounts
bool ConditionEvaluator::checkFluidInputs(const MachineState& state, const Recipe& recipe) {
    for (const auto& ingredient : recipe.inputs) {
        if (!ingredient.isFluid) continue;
        
        auto it = std::find_if(state.fluidInputs.begin(), state.fluidInputs.end(),
            [&](const ItemSlot& slot) { return slot.item_id == ingredient.itemId; });
        
        if (it == state.fluidInputs.end() || it->count < ingredient.count)
            return false;  // insufficient fluid
    }
    return true;
}
```

### 13.4 Update RecipeManager RPC for fluid recipes
**Location**: `src/protocol/recipe.fbs`

```flatbuffers
// Extend CheckRecipeReq with fluid inputs
table CheckRecipeReq {
    machine_type: MachineType;
    inputs: [ItemStack];
    fluid_inputs: [ItemStack];    // NEW
}
```

### 13.5 Fluid consumption/production in machine tick
When a machine processes a fluid recipe:
1. Check fluid inputs available (from pipe network or internal tank)
2. Consume fluid items (remove from pipe or inventory)
3. Produce fluid outputs (push to pipe or inventory)
4. Handle byproducts (optional outputs go to pipe if connected, else void)

## Acceptance Criteria
- [ ] Recipe JSON format supports `"fluid"` and `"amount"` fields
- [ ] `RecipeParser` correctly parses fluid ingredients
- [ ] `ConditionEvaluator::checkFluidInputs()` validates fluid requirements
- [ ] RecipeManager RPC accepts `fluid_inputs` in CheckRecipeReq
- [ ] Machine tick consumes fluid items when recipe starts
- [ ] Machine tick produces fluid outputs on recipe completion
- [ ] Byproduct fluids are optional (machine continues without voiding them)

## Dependencies
- Task 8 (fluid item IDs — registry definitions)
- RecipeManager (existing service)
- Required by: Epic 7 LCR (Large Chemical Reactor)

## Files to Modify
- `data/recipes/` — update recipe JSON schemas
- `src/libs/recipe_manager/` — RecipeParser fluid support
- `src/protocol/recipe.fbs` — fluid_inputs in CheckRecipeReq
- `src/services/simulation_core/Crafting/ConditionEvaluator.cpp` — fluid checks
