# Task: EBF (Electric Blast Furnace)

## Overview
Implement Electric Blast Furnace (EBF) multiblock machine with heating coil tiers, heat requirements, and smelting capabilities. This is the first specialized multiblock machine in the L2 implementation.

## Goal
- Implement complete EBF pattern detection and functionality
- Add heating coil tier system (Kanhal, Nichrome, TungstenSteel)
- Implement heat-based recipe processing
- Add input/output/hatch management
- Integrate with RecipeManager for processing

## Acceptance Criteria
- [ ] EBF pattern correctly detected and registered
- [ ] EBFSystem tick handler implemented
- [ ] Heating coil tier mapping (ID → max heat)
- [ ] Recipe heat requirement validation
- [ ] Input/output/hatch detection and management
- [ ] Muffler hatch functionality
- [ ] Heat-based processing logic
- [ ] Integration with RecipeManager
- [ ] Dissociation support (muffle removal)

## Requirements

### Technical Requirements
- **Language**: C++
- **Location**: `src/services/simulation_core/ECS/systems/`
- **Integration**: With PatternRegistry, RecipeManager, EntityStateStore
- **Performance**: Efficient tick processing (20Hz)

### EBF System Architecture
```cpp
class EBFSystem {
public:
    // Tick processing for all EBF controllers
    void Tick(float deltaTime);
    
    // Get max heat for coil tier
    uint32_t GetMaxHeat(CoilTier tier) const;
    
    // Check if EBF can process (heat requirements met)
    bool CanProcess(const MultiblockController& controller) const;
    
    // Process EBF tick for specific controller
    void ProcessController(const MultiblockController& controller, float deltaTime);
    
    // Get coil tier at position
    CoilTier GetCoilTier(const BlockPos& pos) const;
};
```

### EBF Data Structures
```cpp
enum class CoilTier {
    KANHAL = 1,      // 1800°C max
    NICHROME = 2,     // 2700°C max
    TUNGSTEN_STEEL = 3 // 4500°C max
};

struct EBFController {
    uint64_t controller_id;
    CoilTier max_heat_tier;     // Highest tier coil present
    uint32_t current_heat;      // Current operating temperature
    uint32_t heat_requirement;   // Recipe heat requirement
    uint32_t process_progress;  // Recipe progress (0-100%)
    uint32_t energy_stored;      // EU stored
    bool is_active;              // Currently processing
    bool muffler_installed;      // Top hatch protection
    uint32_t recipe_id;          // Currently processing recipe
    float time_remaining;        // Time left in current process
};
```

### EBF Pattern Definition
```cpp
const MultiblockPattern EBF_PATTERN = {
    .id = 1,
    .name = "Electric Blast Furnace",
    .size_x = 3, .size_y = 3, .size_z = 4,
    .machine_props = EBFProperties{
        .heat_requirement = 128,  // EU/t
        .max_coil_tiers = 3,
        .max_heats = {1800, 2700, 4500},
        .requires_muffler = true
    },
    .hatches = {
        // Input hatches
        HatchDef{Type::INPUT, {-1,0,1}, 0, "Input Slot"},
        HatchDef{Type::INPUT, {-1,1,1}, 0, "Secondary Input"},
        // Output hatches
        HatchDef{Type::OUTPUT, {1,0,1}, 0, "Output Slot"},
        HatchDef{Type::OUTPUT, {1,1,1}, 0, "Secondary Output"},
        // Energy hatch
        HatchDef{Type::ENERGY, {0,0,0}, 0, "Energy Hatch"},
        // Muffler hatch
        HatchDef{Type::MUFFLER, {0,2,3}, 0, "Muffler Hatch"},
        // Heating coils (detected automatically)
        HatchDef{Type::ENERGY, {-1,1,1}, 0, "Kanhal Coil"},
        HatchDef{Type::ENERGY, {0,1,1}, 0, "Nichrome Coil"},
        HatchDef{Type::ENERGY, {1,1,1}, 0, "TungstenSteel Coil"},
    },
    .controller_pos = {{0,1,3}}
};
```

## Implementation Details

### EBF System Tick Logic
```cpp
void EBFSystem::Tick(float deltaTime) {
    for (const auto& controller : registry.view<EBFController>()) {
        ProcessController(controller, deltaTime);
    }
}

void EBFSystem::ProcessController(const MultiblockController& controller, float deltaTime) {
    const auto& ebf = controller.Get<EBFController>();
    
    // Check if muffler is present
    if (!ebf.muffler_installed && !IsMufflerPresent(controller)) {
        // EBF overheats without muffler
        return;
    }
    
    // Check heat availability
    if (!CanProcess(ebf)) {
        return; // Not enough heat
    }
    
    // Process recipe
    ProcessRecipe(ebf, deltaTime);
    
    // Update energy storage
    UpdateEnergyStorage(ebf);
}
```

### Heating Coil Detection
```cpp
CoilTier EBFSystem::GetCoilTier(const BlockPos& pos) const {
    BlockID block_id = chunkStore.GetBlock(pos.x, pos.y, pos.z);
    
    // Map block IDs to coil tiers
    switch (block_id) {
        case BLOCK_KANHAL: return CoilTier::KANHAL;
        case BLOCK_NICHROME: return CoilTier::NICHROME;
        case BLOCK_TUNGSTEN_STEEL: return CoilTier::TUNGSTEN_STEEL;
        default: return CoilTier::KANHAL; // Default tier
    }
}

uint32_t EBFSystem::GetMaxHeat(CoilTier tier) const {
    switch (tier) {
        case CoilTier::KANHAL: return 1800;
        case CoilTier::NICHROME: return 2700;
        case CoilTier::TUNGSTEN_STEEL: return 4500;
        default: return 0;
    }
}
```

### Recipe Processing
```cpp
void EBFSystem::ProcessRecipe(const EBFController& controller, float deltaTime) {
    // Check for available recipe
    RecipeID recipe_id = FindAvailableRecipe(controller);
    if (recipe_id == INVALID_RECIPE) {
        return;
    }
    
    const Recipe& recipe = recipeManager.GetRecipe(recipe_id);
    
    // Check if recipe requires heat
    if (recipe.heat_requirement > controller.max_heat) {
        return; // Not enough heat
    }
    
    // Process recipe
    float progress_increase = (recipe.heat_requirement * deltaTime) / recipe.total_time;
    uint32_t new_progress = controller.process_progress + progress_increase;
    
    if (new_progress >= 100) {
        // Recipe completed
        CompleteRecipe(controller, recipe);
    } else {
        // Update progress
        UpdateRecipeProgress(controller, new_progress);
    }
}
```

### Input/Output Management
```cpp
void EBFSystem::ManageInputs(const MultiblockController& controller) {
    const auto& ebf = controller.Get<EBFController>();
    
    // Check input hatches for items
    for (const auto& hatch : GetHatchesByType(controller, HatchDef::Type::INPUT)) {
        ItemStack input_item = inventoryManager.GetItem(hatch.position);
        if (!input_item.isEmpty() && CanProcessItem(input_item, ebf.recipe_id)) {
            ConsumeInputItem(hatch.position, input_item);
            AddToProcessingQueue(input_item, ebf.recipe_id);
        }
    }
}

void EBFSystem::ManageOutputs(const MultiblockController& controller) {
    const auto& ebf = controller.Get<EBFController>();
    
    // Get output items from processing queue
    std::vector<ItemStack> output_items = GetProcessedItems(ebf.recipe_id);
    
    // Distribute to output hatches
    size_t output_index = 0;
    for (const auto& hatch : GetHatchesByType(controller, HatchDef::Type::OUTPUT)) {
        if (output_index < output_items.size()) {
            inventoryManager.SetItem(hatch.position, output_items[output_index]);
            output_index++;
        }
    }
}
```

## Hatch Detection System
```cpp
class HatchDetector {
public:
    // Detect all hatches for a multiblock controller
    std::vector<HatchDef> DetectHatches(const MultiblockController& controller) const;
    
    // Check if specific hatch type exists
    bool HasHatch(const MultiblockController& controller, HatchDef::Type type) const;
    
    // Get hatch positions for specific type
    std::vector<BlockPos> GetHatchPositions(const MultiblockController& controller, 
                                           HatchDef::Type type) const;
    
    // Check muffler presence (top center)
    bool IsMufflerPresent(const MultiblockController& controller) const;
};
```

## Integration Points

### With RecipeManager
```cpp
// Find available recipe for EBF
RecipeID EBFSystem::FindAvailableRecipe(const EBFController& controller) {
    // Get input items from hatches
    std::vector<ItemStack> inputs = GetInputItems(controller);
    
    // Query RecipeManager for matching recipe
    return recipeManager.FindRecipe(
        RecipeQuery()
            .machineType(MachineType::EBF)
            .inputs(inputs)
            .heatRequirement(controller.max_heat)
    );
}
```

### With EntityStateStore
```cpp
// Persist EBF controller state
void EBFSystem::PersistControllerState(const MultiblockController& controller) {
    EBFControllerState state = {
        .controller_id = controller.id,
        .max_heat_tier = controller.Get<EBFController>().max_heat_tier,
        .current_heat = controller.Get<EBFController>().current_heat,
        .process_progress = controller.Get<EBFController>().process_progress,
        .energy_stored = controller.Get<EBFController>().energy_stored,
        .is_active = controller.Get<EBFController>().is_active,
        .muffler_installed = controller.Get<EBFController>().muffler_installed,
        .recipe_id = controller.Get<EBFController>().recipe_id,
        .time_remaining = controller.Get<EBFController>().time_remaining
    };
    
    entityStateStore.SetEntityState(controller.entity_id, state);
}
```

## Files to Modify
- `src/services/simulation_core/ECS/systems/EBFSystem.h` - New
- `src/services/simulation_core/ECS/systems/EBFSystem.cpp` - New
- `src/services/simulation_core/ECS/components/EBFController.h` - New
- `src/services/simulation_core/ECS/components/CoilTier.h` - New
- `src/services/simulation_core/ECS/components/HatchDetector.h` - New
- `src/services/simulation_core/src/PatternMatcher.cpp` - Updated
- `src/services/simulation_core/src/SimulationEngine.cpp` - Updated

## Testing Strategy
- Unit tests for heating coil tier mapping
- Integration tests with RecipeManager
- EBF processing logic tests
- Hatch detection accuracy tests
- Muffler functionality tests
- Dissociation tests (muffle removal)

## Success Metrics
- EBF pattern detected correctly
- Heating coil tiers functional
- Recipe processing working
- Input/output management accurate
- Muffler protection effective
- Heat-based processing accurate
- Integration with RecipeManager complete