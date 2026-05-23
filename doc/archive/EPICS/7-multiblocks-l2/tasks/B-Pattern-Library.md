# Task: Pattern Library (Generic)

## Overview
Implement generic multiblock pattern detection system supporting EBF, Large Boiler, and LCR patterns. This provides the foundation for all multiblock machines.

## Goal
- Create MultiblockPattern struct with comprehensive pattern definition
- Implement pattern registry in SimulationCore
- Create generic matchPattern() function for all pattern types
- Support hatch definitions and controller positions

## Acceptance Criteria
- [ ] MultiblockPattern struct fully defined
- [ ] Pattern registry supports 3+ machine types
- [ ] Generic matchPattern() function implemented
- [ ] Hatch detection system working
- [ ] Controller position validation
- [ ] Pattern serialization for persistence
- [ ] Unit tests for all pattern types

## Requirements

### Technical Requirements
- **Language**: C++
- **Location**: `src/services/simulation_core/ECS/components/`
- **Integration**: With SimulationCore and SpatialIndex
- **Persistence**: Pattern definitions in EntityStateStore

### Data Structures

#### MultiblockPattern
```cpp
struct MultiblockPattern {
    uint32_t id;               // Machine type ID (1=EBF, 2=Boiler, 3=LCR)
    std::string name;          // Human-readable name
    uint8_t size_x, size_y, size_z;  // Pattern dimensions
    
    // Pattern definition
    std::vector<PatternLayer> layers;  // 3D block layout
    
    // Hatch definitions (input/output/energy/fluid/item)
    std::vector<HatchDef> hatches;
    
    // Controller positions (where multiblock controller spawns)
    std::vector<BlockPos> controller_pos;
    
    // Machine-specific properties
    std::variant<EBFProperties, BoilerProperties, LCRProperties> machine_props;
};
```

#### PatternLayer
```cpp
struct PatternLayer {
    std::array<BlockID, PATTERN_SIZE> blocks;  // Flattened 3D array
    std::array<bool, PATTERN_SIZE> is_controller;  // Controller positions
    std::array<bool, PATTERN_SIZE> is_hatch;  // Hatch positions
};
```

#### HatchDef
```cpp
struct HatchDef {
    enum class Type { INPUT, OUTPUT, ENERGY, FLUID, ITEM, MUFFLER };
    BlockPos position;
    uint32_t priority;  // Processing order
    std::string name;   // Display name
    std::variant<ItemFilter, FluidFilter, EnergyFilter> requirements;
};
```

### Pattern Registry
```cpp
class PatternRegistry {
public:
    // Register new pattern
    void RegisterPattern(const MultiblockPattern& pattern);
    
    // Get pattern by ID
    const MultiblockPattern* GetPattern(uint32_t id) const;
    
    // Get all patterns
    std::vector<const MultiblockPattern*> GetAllPatterns() const;
    
    // Get patterns by machine type
    std::vector<const MultiblockPattern*> GetPatternsByType(MachineType type) const;
    
    // Validate pattern completeness
    bool ValidatePattern(const MultiblockPattern& pattern) const;
};
```

### Generic Pattern Matching
```cpp
class PatternMatcher {
public:
    // Match pattern at position (requires SpatialIndex)
    bool MatchPattern(const MultiblockPattern& pattern, 
                     uint32_t x, uint32_t y, uint32_t z,
                     const std::function<bool(uint32_t, uint32_t, uint32_t)>& blockValidator);
    
    // Check if pattern can be completed (partial match)
    bool CanCompletePattern(const MultiblockPattern& pattern,
                           uint32_t x, uint32_t y, uint32_t z);
    
    // Find all possible pattern placements
    std::vector<BlockPos> FindPatternPlacements(const MultiblockPattern& pattern);
};
```

## Machine-Specific Properties

### EBFProperties
```cpp
struct EBFProperties {
    uint32_t heat_requirement;  // EU/t required
    uint32_t max_coil_tiers;   // Kanhal, Nichrome, TungstenSteel
    std::array<uint32_t, 3> max_heats;  // Per tier
    bool requires_muffler;     // Top hatch protection
};
```

### BoilerProperties
```cpp
struct BoilerProperties {
    std::array<FuelType, MAX_FUEL_SLOTS> supported_fuels;
    uint32_t water_capacity;   // mB
    uint32_t steam_output_rate; // mB/t
    uint32_t heat_capacity;    // Temperature rise per fuel
    bool multi_size_support;   // 1×1×1 to 3×3×4
};
```

### LCRProperties
```cpp
struct LCRProperties {
    uint32_t energy_requirement;  // EU/t
    std::vector<RecipeID> supported_recipes;
    uint32_t fluid_input_capacity;  // mB
    uint32_t item_input_capacity;   // Slots
    bool fluid_output_supported;    // Byproducts
};
```

## Pattern Definitions

### EBF Pattern (3×3×4)
```cpp
const MultiblockPattern EBF_PATTERN = {
    .id = 1,
    .name = "Electric Blast Furnace",
    .size_x = 3, .size_y = 3, .size_z = 4,
    .machine_props = EBFProperties{
        .heat_requirement = 128,
        .max_coil_tiers = 3,
        .max_heats = {1800, 2700, 4500},
        .requires_muffler = true
    },
    .hatches = {
        HatchDef{Type::INPUT, {-1,0,1}, 0, "Input"},
        HatchDef{Type::OUTPUT, {1,0,1}, 0, "Output"},
        HatchDef{Type::ENERGY, {0,0,0}, 0, "Energy"},
        HatchDef{Type::MUFFLER, {0,2,3}, 0, "Muffler"},
        // Heating coils at y=1,z=1 and y=2,z=1
    },
    .controller_pos = {{0,1,3}}
};
```

### Large Boiler Pattern (3×3×4)
```cpp
const MultiblockPattern BOILER_PATTERN = {
    .id = 2,
    .name = "Large Steam Boiler",
    .size_x = 3, .size_y = 3, .size_z = 4,
    .machine_props = BoilerProperties{
        .supported_fuels = {FuelType::COAL, FuelType::CHARCOAL, FuelType::FURNACE_OIL},
        .water_capacity = 16000,
        .steam_output_rate = 400,
        .heat_capacity = 200,
        .multi_size_support = true
    },
    .hatches = {
        HatchDef{Type::INPUT, {-1,1,0}, 0, "Fuel"},
        HatchDef{Type::INPUT, {0,1,0}, 0, "Water"},
        HatchDef{Type::OUTPUT, {1,1,0}, 0, "Steam"},
        HatchDef{Type::ENERGY, {0,0,0}, 0, "Energy"},
    },
    .controller_pos = {{0,1,3}}
};
```

### LCR Pattern (3×3×3)
```cpp
const MultiblockPattern LCR_PATTERN = {
    .id = 3,
    .name = "Large Chemical Reactor",
    .size_x = 3, .size_y = 3, .size_z = 3,
    .machine_props = LCRProperties{
        .energy_requirement = 64,
        .supported_recipes = {RECIPE_SULFURIC_ACID_IRON, RECIPE_CHEMICAL_PROCESSING},
        .fluid_input_capacity = 4000,
        .item_input_capacity = 4,
        .fluid_output_supported = true
    },
    .hatches = {
        HatchDef{Type::INPUT, {-1,0,0}, 0, "Item Input"},
        HatchDef{Type::INPUT, {1,0,0}, 0, "Fluid Input"},
        HatchDef{Type::OUTPUT, {0,0,0}, 0, "Item Output"},
        HatchDef{Type::OUTPUT, {0,0,1}, 0, "Fluid Output"},
        HatchDef{Type::ENERGY, {0,1,0}, 0, "Energy"},
    },
    .controller_pos = {{0,1,2}}
};
```

## Pattern Detection Flow
```cpp
// Integration with SimulationCore
void PatternRegistry::Initialize() {
    RegisterPattern(EBF_PATTERN);
    RegisterPattern(BOILER_PATTERN);
    RegisterPattern(LCR_PATTERN);
}

void SimulationCore::OnBlockChanged(const BlockPos& pos, BlockID newBlock) {
    if (newBlock == AIR) {
        // Check for dissociation
        CheckForDissociation(pos);
        return;
    }
    
    // Try to match all patterns at this position
    for (const auto& pattern : patternRegistry.GetAllPatterns()) {
        if (patternMatcher.MatchPattern(*pattern, pos.x, pos.y, pos.z, 
                                         [this](uint32_t x, uint32_t y, uint32_t z) {
                                             return chunkStore.GetBlock(x, y, z);
                                         })) {
            CreateMultiblockController(*pattern, pos.x, pos.y, pos.z);
            return;
        }
    }
}
```

## Files to Modify
- `src/services/simulation_core/ECS/components/MultiblockController.h` - Enhanced
- `src/services/simulation_core/ECS/components/MultiblockPattern.h` - New
- `src/services/simulation_core/ECS/components/HatchDef.h` - New
- `src/services/simulation_core/ECS/components/PatternRegistry.h` - New
- `src/services/simulation_core/src/PatternMatcher.cpp` - New
- `src/services/simulation_core/src/PatternRegistry.cpp` - New

## Testing Strategy
- Unit tests for pattern validation
- Integration tests with SpatialIndex
- Pattern matching accuracy tests
- Machine-specific property validation

## Success Metrics
- All 3 patterns correctly detected
- No false positives/negatives
- Hatch detection accuracy 100%
- Controller placement validation
- Performance < 10ms for pattern matching