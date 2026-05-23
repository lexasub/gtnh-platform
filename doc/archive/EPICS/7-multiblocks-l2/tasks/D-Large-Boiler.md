# Task: Large Steam Boiler

## Overview
Implement Large Steam Boiler (LSB) multiblock machine with fuel burning, water-steam conversion, and heat management. This provides high-temperature steam for piping systems.

## Goal
- Implement complete Large Steam Boiler pattern detection and functionality
- Add fuel burning system (coal, charcoal, fluid fuels)
- Implement water-to-steam conversion
- Add overheat detection and damage
- Support multi-size variants (1×1×1 to 3×3×4)
- Integrate with PipeNetwork for steam distribution

## Acceptance Criteria
- [ ] Large Boiler pattern correctly detected and registered
- [ ] Boiler tick handler implemented
- [ ] Fuel burning system working (coal, charcoal, fluid)
- [ ] Water input to steam conversion
- [ ] Steam output to PipeNetwork
- [ ] Overheat detection and damage
- [ ] Multi-size support (1×1×1 to 3×3×4)
- [ ] Integration with PipeNetwork
- [ ] Fuel inventory management
- [ ] Heat capacity and thermal mass

## Requirements

### Technical Requirements
- **Language**: C++
- **Location**: `src/services/simulation_core/ECS/systems/`
- **Integration**: With PatternRegistry, PipeNetwork, EntityStateStore
- **Performance**: Efficient tick processing with heat simulation

### Boiler System Architecture
```cpp
class LargeBoilerSystem {
public:
    // Tick processing for all boiler controllers
    void Tick(float deltaTime);
    
    // Get fuel type at position
    FuelType GetFuelType(const BlockPos& pos) const;
    
    // Check if boiler can burn fuel
    bool CanBurnFuel(const LargeBoilerController& controller) const;
    
    // Process boiler tick for specific controller
    void ProcessController(const MultiblockController& controller, float deltaTime);
    
    // Get water storage
    uint32_t GetWaterStorage(const LargeBoilerController& controller) const;
    
    // Get steam output rate
    uint32_t GetSteamOutputRate(const LargeBoilerController& controller) const;
};
```

### Boiler Data Structures
```cpp
enum class FuelType {
    COAL = 1,\n    CHARCOAL = 2,\n    FURNACE_OIL = 3,\n    LAVA = 4,\n    PLANK = 5
};

struct LargeBoilerController {
    uint64_t controller_id;\n    uint32_t boiler_size;      // 1 (small), 2 (medium), 3 (large)
    uint32_t current_heat;      // Current temperature (0-4000°)
    uint32_t max_heat;          // Maximum temperature (depends on size)
    uint32_t water_storage;      // mB water stored (max 16000)
    uint32_t steam_output;       // mB/t steam output rate
    uint32_t fuel_slots;         // Number of fuel slots (depends on size)
    uint32_t current_fuel;       // Current fuel item
    uint32_t fuelburn_time;      // Time remaining in current fuel
    uint32_t heat_capacity;      // Heat capacity (higher for larger boilers)
    bool is_overheated;          // Overheat state
    float overheat_timer;        // Time to cooldown
    uint32_t controller_x, controller_y, controller_z;  // Anchor position
};
```

### Boiler Pattern Definition
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
        // Fuel input hatches
        HatchDef{Type::INPUT, {-1,1,0}, 0, "Fuel Slot 1"},
        HatchDef{Type::INPUT, {0,1,0}, 0, "Fuel Slot 2"},
        HatchDef{Type::INPUT, {1,1,0}, 0, "Fuel Slot 3"},
        // Water input hatches
        HatchDef{Type::INPUT, {-1,0,0}, 0, "Water Input 1"},
        HatchDef{Type::INPUT, {0,0,0}, 0, "Water Input 2"},
        HatchDef{Type::INPUT, {1,0,0}, 0, "Water Input 3"},
        // Steam output hatches
        HatchDef{Type::OUTPUT, {-1,2,0}, 0, "Steam Output 1"},
        HatchDef{Type::OUTPUT, {0,2,0}, 0, "Steam Output 2"},
        HatchDef{Type::OUTPUT, {1,2,0}, 0, "Steam Output 3"},
        // Energy hatch
        HatchDef{Type::ENERGY, {0,0,0}, 0, "Energy Hatch"},
        // Controller position
        .controller_pos = {{0,1,3}}
    }
};
```

## Implementation Details

### Boiler System Tick Logic
```cpp
void LargeBoilerSystem::Tick(float deltaTime) {
    for (const auto& controller : registry.view<LargeBoilerController>()) {
        ProcessController(controller, deltaTime);
    }
}

void LargeBoilerSystem::ProcessController(const MultiblockController& controller, float deltaTime) {
    auto& boiler = controller.Get<LargeBoilerController>();
    
    // Check for overheating
    if (boiler.current_heat >= boiler.max_heat) {
        HandleOverheat(boiler);
        return; // Skip processing when overheated
    }
    
    // Try to burn fuel
    if (TryBurnFuel(boiler, deltaTime)) {
        // Update heat
        boiler.current_heat = std::min(boiler.current_heat + GetHeatPerTick(boiler), boiler.max_heat);
    }
    
    // Process water conversion
    ProcessWaterConversion(boiler, deltaTime);
    
    // Update overheat timer
    if (boiler.is_overheated) {
        boiler.overheat_timer -= deltaTime;
        if (boiler.overheat_timer <= 0) {
            boiler.is_overheated = false;
        }
    }
}
```

### Fuel Burning System
```cpp
bool LargeBoilerSystem::TryBurnFuel(LargeBoilerController& controller, float deltaTime) {
    if (controller.current_fuel == INVALID_ITEM) {
        // Try to get new fuel
        controller.current_fuel = FindFuelInHatches(controller);
        if (controller.current_fuel == INVALID_ITEM) {
            return false; // No fuel
        }
        
        // Get fuel burn time
        controller.fuelburn_time = GetFuelBurnTime(controller.current_fuel);
    }
    
    // Burn fuel for this tick
    controller.fuelburn_time -= deltaTime;
    
    if (controller.fuelburn_time <= 0) {
        // Fuel depleted
        ConsumeFuel(controller.current_fuel);
        controller.current_fuel = INVALID_ITEM;
        return false;
    }
    
    return true;
}

FuelType LargeBoilerSystem::GetFuelType(const BlockPos& pos) const {
    BlockID block_id = chunkStore.GetBlock(pos.x, pos.y, pos.z);
    
    switch (block_id) {
        case BLOCK_COAL: return FuelType::COAL;
        case BLOCK_CHARCOAL: return FuelType::CHARCOAL;
        case BLOCK_FURNACE_OIL: return FuelType::FURNACE_OIL;
        case BLOCK_LAVA: return FuelType::LAVA;
        case BLOCK_PLANKS: return FuelType::PLANK;
        default: return FuelType::COAL; // Default fuel
    }
}

uint32_t LargeBoilerSystem::GetFuelBurnTime(FuelID fuel_id) const {
    switch (GetFuelTypeFromID(fuel_id)) {
        case FuelType::COAL: return 200;  // 200 ticks
        case FuelType::CHARCOAL: return 400;  // 400 ticks
        case FuelType::FURNACE_OIL: return 100;  // 100 ticks
        case FuelType::LAVA: return 20;  // 20 ticks
        case FuelType::PLANK: return 50;  // 50 ticks
        default: return 100; // Default
    }
}
```

### Water to Steam Conversion
```cpp
void LargeBoilerSystem::ProcessWaterConversion(LargeBoilerController& controller, float deltaTime) {
    if (controller.current_heat < 1000) {
        return; // Not hot enough
    }
    
    uint32_t water_per_tick = GetWaterPerTick(controller);
    if (water_per_tick <= 0) {
        return;
    }
    
    // Convert water to steam
    uint32_t steam_produced = std::min(water_per_tick, controller.water_storage);
    controller.water_storage -= steam_produced;
    
    // Output steam to PipeNetwork
    OutputSteam(controller, steam_produced);
}

uint32_t LargeBoilerSystem::GetWaterPerTick(const LargeBoilerController& controller) const {
    // Higher heat = more steam
    return (controller.current_heat / 100) * controller.steam_output;
}
```

### Overheat Detection
```cpp
void LargeBoilerSystem::HandleOverheat(LargeBoilerController& controller) {
    controller.is_overheated = true;
    controller.overheat_timer = OVERHEAT_COOLDOWN_TIME;
    
    // Damage boiler on overheat
    if (controller.boiler_size >= 2) {  // Larger boilers take more damage
        DamageBoiler(controller);
    }
}

void LargeBoilerSystem::DamageBoiler(LargeBoilerController& controller) {
    // Reduce boiler size or capacity
    if (controller.boiler_size > 1) {
        controller.boiler_size--;
        controller.max_heat = CalculateMaxHeat(controller.boiler_size);
        controller.heat_capacity = CalculateHeatCapacity(controller.boiler_size);
    }
    
    // Break controller block
    BreakControllerBlock(controller);
}
```

### Multi-Size Support
```cpp
void LargeBoilerSystem::InitializeController(LargeBoilerController& controller, uint32_t size) {
    controller.boiler_size = size;
    controller.max_heat = CalculateMaxHeat(size);
    controller.heat_capacity = CalculateHeatCapacity(size);
    controller.fuel_slots = size * size;  // More slots for larger boilers
    controller.water_storage = size * size * 4000;  // More water for larger boilers
    controller.steam_output = size * size * 100;  // More steam output for larger boilers
}

uint32_t LargeBoilerSystem::CalculateMaxHeat(uint32_t size) const {
    return 1000 + (size - 1) * 500;  // 1000°C for size 1, 1500°C for size 2, 2000°C for size 3
}

uint32_t LargeBoilerSystem::CalculateHeatCapacity(uint32_t size) const {
    return 100 * size * size;  // Higher capacity for larger boilers
}
```

## Integration Points

### With PipeNetwork
```cpp
// Output steam to PipeNetwork
void LargeBoilerSystem::OutputSteam(const LargeBoilerController& controller, uint32_t steam_amount) {
    for (const auto& hatch : GetHatchesByType(controller, HatchDef::Type::OUTPUT)) {
        // Send steam to PipeNetwork
        pipeNetwork.PublishFluid(hatch.position, FluidType::STEAM, steam_amount / 3);
    }
}
```

### With EntityStateStore
```cpp
// Persist boiler controller state
void LargeBoilerSystem::PersistControllerState(const MultiblockController& controller) {
    LargeBoilerControllerState state = {
        .controller_id = controller.id,
        .boiler_size = controller.Get<LargeBoilerController>().boiler_size,
        .current_heat = controller.Get<LargeBoilerController>().current_heat,
        .max_heat = controller.Get<LargeBoilerController>().max_heat,
        .water_storage = controller.Get<LargeBoilerController>().water_storage,
        .steam_output = controller.Get<LargeBoilerController>().steam_output,
        .fuel_slots = controller.Get<LargeBoilerController>().fuel_slots,
        .current_fuel = controller.Get<LargeBoilerController>().current_fuel,
        .fuelburn_time = controller.Get<LargeBoilerController>().fuelburn_time,
        .heat_capacity = controller.Get<LargeBoilerController>().heat_capacity,
        .is_overheated = controller.Get<LargeBoilerController>().is_overheated,
        .overheat_timer = controller.Get<LargeBoilerController>().overheat_timer
    };
    
    entityStateStore.SetEntityState(controller.entity_id, state);
}
```

## Files to Modify
- `src/services/simulation_core/ECS/systems/LargeBoilerSystem.h` - New
- `src/services/simulation_core/ECS/systems/LargeBoilerSystem.cpp` - New
- `src/services/simulation_core/ECS/components/LargeBoilerController.h` - New
- `src/services/simulation_core/ECS/components/FuelType.h` - New
- `src/services/simulation_core/src/PatternMatcher.cpp` - Updated
- `src/services/simulation_core/src/SimulationEngine.cpp` - Updated

## Testing Strategy
- Unit tests for fuel burning system
- Integration tests with PipeNetwork
- Water-steam conversion tests
- Overheat detection tests
- Multi-size support tests
- Damage system tests

## Success Metrics
- Boiler pattern detected correctly
- Fuel burning working
- Water-steam conversion functional
- Overheat detection accurate
- Multi-size support working
- Steam output to PipeNetwork
- Integration with PipeNetwork complete