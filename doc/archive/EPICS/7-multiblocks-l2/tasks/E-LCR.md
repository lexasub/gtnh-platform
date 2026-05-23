# Task: LCR (Large Chemical Reactor)

## Overview
Implement Large Chemical Reactor (LCR) multiblock machine with fluid and item processing capabilities. This provides chemical processing for the GTNH ecosystem.

## Goal
- Implement complete LCR pattern detection and functionality
- Add fluid and solid input processing
- Integrate with RecipeManager for chemical reactions
- Implement byproduct handling
- Add fluid hatch detection and management
- Support chemical reactions (e.g., sulfuric acid + iron → iron sulfate)

## Acceptance Criteria
- [ ] LCR pattern correctly detected and registered
- [ ] LCR tick handler implemented
- [ ] RecipeManager: fluid + solid input recipes
- [ ] Fluid hatch detection and management
- [ ] Byproduct handling (fluid output)
- [ ] Energy requirement validation
- [ ] Chemical reaction processing
- [ ] Integration with RecipeManager
- [ ] EntityStateStore persistence
- [ ] Input/output management

## Requirements

### Technical Requirements
- **Language**: C++
- **Location**: `src/services/simulation_core/ECS/systems/`
- **Integration**: With PatternRegistry, RecipeManager, EntityStateStore
- **Performance**: Efficient chemical reaction processing (20Hz)

### LCR System Architecture
```cpp
class LCRSystem {
public:
    // Tick processing for all LCR controllers
    void Tick(float deltaTime);
    
    // Check if LCR can process
    bool CanProcess(const LargeChemicalReactorController& controller) const;
    
    // Process LCR tick for specific controller
    void ProcessController(const MultiblockController& controller, float deltaTime);
    
    // Get fluid storage
    uint32_t GetFluidStorage(const LargeChemicalReactorController& controller) const;
    
    // Get item storage
    uint32_t GetItemStorage(const LargeChemicalReactorController& controller) const;
};
```

### LCR Data Structures
```cpp
struct LargeChemicalReactorController {
    uint64_t controller_id;
    uint32_t energy_stored;      // EU stored
    uint32_t energy_requirement;  // EU/t required
    uint32_t fluid_input_capacity;  // mB fluid input capacity
    uint32_t item_input_capacity;   // Slots item input capacity
    uint32_t fluid_output_capacity; // mB fluid output capacity
    uint32_t item_output_capacity;  // Slots item output capacity
    uint32_t current_fluid;        // Current fluid item
    uint32_t current_item;         // Current item
    uint32_t process_progress;     // Recipe progress (0-100%)
    bool is_processing;            // Currently processing
    uint32_t recipe_id;            // Currently processing recipe
    float time_remaining;          // Time left in current process
    std::vector<FluidStack> fluid_inputs;
    std::vector<ItemStack> item_inputs;
    std::vector<FluidStack> fluid_outputs;
    std::vector<ItemStack> item_outputs;
};
```

### LCR Pattern Definition
```cpp
const MultiblockPattern LCR_PATTERN = {
    .id = 3,
    .name = "Large Chemical Reactor",
    .size_x = 3, .size_y = 3, .size_z = 3,
    .machine_props = LCRProperties{
        .energy_requirement = 64,  // EU/t
        .supported_recipes = {RECIPE_SULFURIC_ACID_IRON, RECIPE_CHEMICAL_PROCESSING},
        .fluid_input_capacity = 4000,
        .item_input_capacity = 4,
        .fluid_output_supported = true
    },
    .hatches = {
        // Item input hatches
        HatchDef{Type::INPUT, {-1,0,0}, 0, "Item Input 1"},
        HatchDef{Type::INPUT, {0,0,0}, 0, "Item Input 2"},
        HatchDef{Type::INPUT, {1,0,0}, 0, "Item Input 3"},
        // Fluid input hatches
        HatchDef{Type::INPUT, {-1,0,1}, 0, "Fluid Input 1"},
        HatchDef{Type::INPUT, {0,0,1}, 0, "Fluid Input 2"},
        HatchDef{Type::INPUT, {1,0,1}, 0, "Fluid Input 3"},
        // Item output hatches
        HatchDef{Type::OUTPUT, {-1,1,0}, 0, "Item Output 1"},
        HatchDef{Type::OUTPUT, {0,1,0}, 0, "Item Output 2"},
        HatchDef{Type::OUTPUT, {1,1,0}, 0, "Item Output 3"},
        // Fluid output hatches
        HatchDef{Type::OUTPUT, {-1,1,1}, 0, "Fluid Output 1"},
        HatchDef{Type::OUTPUT, {0,1,1}, 0, "Fluid Output 2"},
        HatchDef{Type::OUTPUT, {1,1,1}, 0, "Fluid Output 3"},
        // Energy hatch
        HatchDef{Type::ENERGY, {0,1,2}, 0, "Energy Hatch"},
    },
    .controller_pos = {{0,1,2}}
};
```

## Implementation Details

### LCR System Tick Logic
```cpp
void LCRSystem::Tick(float deltaTime) {
    for (const auto& controller : registry.view<LargeChemicalReactorController>()) {
        ProcessController(controller, deltaTime);
    }
}

void LCRSystem::ProcessController(const MultiblockController& controller, float deltaTime) {
    auto& lcr = controller.Get<LargeChemicalReactorController>();
    
    // Check if can process
    if (!CanProcess(lcr)) {
        return;
    }
    
    // Process recipe
    ProcessRecipe(lcr, deltaTime);
    
    // Update energy storage
    UpdateEnergyStorage(lcr);
    
    // Update fluid and item inputs/outputs
    ManageInputsAndOutputs(lcr);
}
```

### Recipe Processing
```cpp
void LCRSystem::ProcessRecipe(LargeChemicalReactorController& controller, float deltaTime) {
    // Check for available recipe
    RecipeID recipe_id = FindAvailableRecipe(controller);
    if (recipe_id == INVALID_RECIPE) {
        return;
    }
    
    const Recipe& recipe = recipeManager.GetRecipe(recipe_id);
    
    // Check if recipe requires energy
    if (recipe.energy_requirement > controller.energy_stored) {
        return; // Not enough energy
    }
    
    // Process recipe
    float progress_increase = (recipe.energy_requirement * deltaTime) / recipe.total_time;
    controller.process_progress += progress_increase;
    
    if (controller.process_progress >= 100) {
        // Recipe completed
        CompleteRecipe(controller, recipe);
    } else {
        // Update progress
        UpdateRecipeProgress(controller, recipe);
    }
}
```

### Fluid Management
```cpp
void LCRSystem::ManageFluidInputs(LargeChemicalReactorController& controller) {
    // Collect fluid inputs from hatches
    for (const auto& hatch : GetHatchesByType(controller, HatchDef::Type::INPUT)) {
        // Check if this is a fluid input hatch
        if (IsFluidHatch(hatch)) {
            FluidStack fluid = GetFluidFromHatch(hatch.position);
            if (!fluid.isEmpty() && CanProcessFluid(fluid, controller.recipe_id)) {
                controller.fluid_inputs.push_back(fluid);
                ConsumeFluid(hatch.position, fluid);
            }
        }
    }
}

void LCRSystem::ManageFluidOutputs(LargeChemicalReactorController& controller) {
    // Get output fluids from processed recipes
    std::vector<FluidStack> output_fluids = GetFluidOutputs(controller.recipe_id);
    
    // Distribute to fluid output hatches
    size_t output_index = 0;
    for (const auto& hatch : GetHatchesByType(controller, HatchDef::Type::OUTPUT)) {
        if (IsFluidHatch(hatch) && output_index < output_fluids.size()) {
            SetFluidInHatch(hatch.position, output_fluids[output_index]);
            output_index++;
        }
    }
}
```

### Item Management
```cpp
void LCRSystem::ManageItemInputs(LargeChemicalReactorController& controller) {
    // Collect item inputs from hatches
    for (const auto& hatch : GetHatchesByType(controller, HatchDef::Type::INPUT)) {
        // Check if this is an item input hatch
        if (IsItemHatch(hatch)) {
            ItemStack item = GetItemFromHatch(hatch.position);
            if (!item.isEmpty() && CanProcessItem(item, controller.recipe_id)) {
                controller.item_inputs.push_back(item);
                ConsumeItem(hatch.position, item);
            }
        }
    }
}

void LCRSystem::ManageItemOutputs(LargeChemicalReactorController& controller) {
    // Get output items from processed recipes
    std::vector<ItemStack> output_items = GetItemOutputs(controller.recipe_id);
    
    // Distribute to item output hatches
    size_t output_index = 0;
    for (const auto& hatch : GetHatchesByType(controller, HatchDef::Type::OUTPUT)) {
        if (IsItemHatch(hatch) && output_index < output_items.size()) {
            SetItemInHatch(hatch.position, output_items[output_index]);
            output_index++;
        }
    }
}
```

## Recipe Integration
```cpp
// Find available recipe for LCR
RecipeID LCRSystem::FindAvailableRecipe(const LargeChemicalReactorController& controller) {
    // Get all inputs
    std::vector<ItemStack> item_inputs = GetItemInputsFromHatches(controller);
    std::vector<FluidStack> fluid_inputs = GetFluidInputsFromHatches(controller);
    
    // Query RecipeManager for matching recipe
    return recipeManager.FindRecipe(
        RecipeQuery()
            .machineType(MachineType::LCR)
            .itemInputs(item_inputs)
            .fluidInputs(fluid_inputs)
            .energyRequirement(controller.energy_requirement)
    );
}
```

## Integration Points

### With RecipeManager
```cpp
// Complete recipe processing
void LCRSystem::CompleteRecipe(const LargeChemicalReactorController& controller, const Recipe& recipe) {
    // Get recipe outputs
    std::vector<ItemStack> item_outputs = recipe.GetItemOutputs();
    std::vector<FluidStack> fluid_outputs = recipe.GetFluidOutputs();
    
    // Store outputs for hatch distribution
    StoreRecipeOutputs(controller.recipe_id, item_outputs, fluid_outputs);
    
    // Reset progress
    ResetRecipeProgress(controller);
    
    // Consume resources
    ConsumeRecipeResources(controller, recipe);
}
```

### With EntityStateStore
```cpp
// Persist LCR controller state
void LCRSystem::PersistControllerState(const MultiblockController& controller) {
    LargeChemicalReactorControllerState state = {
        .controller_id = controller.id,
        .energy_stored = controller.Get<LargeChemicalReactorController>().energy_stored,
        .energy_requirement = controller.Get<LargeChemicalReactorController>().energy_requirement,
        .fluid_input_capacity = controller.Get<LargeChemicalReactorController>().fluid_input_capacity,
        .item_input_capacity = controller.Get<LargeChemicalReactorController>().item_input_capacity,
        .fluid_output_capacity = controller.Get<LargeChemicalReactorController>().fluid_output_capacity,
        .item_output_capacity = controller.Get<LargeChemicalReactorController>().item_output_capacity,
        .current_fluid = controller.Get<LargeChemicalReactorController>().current_fluid,
        .current_item = controller.Get<LargeChemicalReactorController>().current_item,
        .process_progress = controller.Get<LargeChemicalReactorController>().process_progress,
        .is_processing = controller.Get<LargeChemicalReactorController>().is_processing,
        .recipe_id = controller.Get<LargeChemicalReactorController>().recipe_id,
        .time_remaining = controller.Get<LargeChemicalReactorController>().time_remaining
    };
    
    entityStateStore.SetEntityState(controller.entity_id, state);
}
```

## Files to Modify
- `src/services/simulation_core/ECS/systems/LCRSystem.h` - New
- `src/services/simulation_core/ECS/systems/LCRSystem.cpp` - New
- `src/services/simulation_core/ECS/components/LargeChemicalReactorController.h` - New
- `src/services/simulation_core/ECS/components/RecipeQuery.h` - New
- `src/services/simulation_core/src/PatternMatcher.cpp` - Updated
- `src/services/simulation_core/src/SimulationEngine.cpp` - Updated

## Testing Strategy
- Unit tests for recipe processing
- Integration tests with RecipeManager
- Fluid and item management tests
- Chemical reaction validation
- Byproduct handling tests
- Energy requirement validation

## Success Metrics
- LCR pattern detected correctly
- Recipe processing working
- Fluid and item inputs managed
- Chemical reactions processed
- Byproduct handling functional
- Energy requirements validated
- Integration with RecipeManager complete