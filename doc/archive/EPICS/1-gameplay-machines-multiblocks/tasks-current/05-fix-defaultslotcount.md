# Task 5: Implement defaultMachineSlotCount() from MachineRegistry

## Objective
Replace hardcoded slot counts with dynamic lookup from MachineRegistry based on machine type and configuration.

## Requirements

### 5.1 Locate and fix defaultMachineSlotCount() function
**Location**: `src/services/simulation_core/ECS/SimulationEngine.h` / `.cpp`

**Current Implementation** (to be replaced):
```cpp
// Current hardcoded version (example):
int defaultMachineSlotCount(uint16_t id) {
    if (id == 36) return 1; // heat_furnace
    if (id == 48) return 1; // heat_macerator
    if (id == 52) return 1; // steam_compressor
    // ... many hardcoded values
    return 0;
}
```

**Required Implementation**:
```cpp
// Updated to use MachineRegistry:
int defaultMachineSlotCount(uint16_t id) {
    const MachineInfo* info = MachineRegistry::getMachineInfo(id);
    if (info == nullptr) return 0;
    return info->slots_in + info->slots_out;
}
```

### 5.2 Update MachineRegistry to include slot counts
**Location**: `src/services/simulation_core/ECS/MachineRegistry.h`

**Implementation** (ensure MachineInfo has slot fields):
```cpp
struct MachineInfo {
    uint16_t id;
    std::string name;
    MachineType type;
    EnergyType energy;
    int slots_in;      // Add this field
    int slots_out;     // Add this field
    int capacity;
    int tier;
    int maxInput;
    int maxOutput;
};
```

### 5.3 Update all callers of defaultMachineSlotCount()
**Location**: `src/services/simulation_core/ECS/SimulationEngine.cpp`

**Implementation**:
```cpp
// Replace all defaultMachineSlotCount(id) calls with:
int totalSlots = info->slots_in + info->slots_out;

// Or if still using the function:
int totalSlots = defaultMachineSlotCount(blockId);
```

### 5.4 Update SimulationEngine::createMachineEntity()
**Location**: `src/services/simulation_core/ECS/SimulationEngine.cpp`

**Implementation**:
```cpp
void SimulationEngine::createMachineEntity(const Vec3i& pos, 
                                          uint16_t blockId,
                                          const MachineInfo& info) {
    // OLD:
    int totalSlots = defaultMachineSlotCount(blockId);
    
    // NEW (using MachineInfo directly):
    int totalSlots = info.slots_in + info.slots_out;
    
    // Create entities with correct slot count
    entt::entity machineEntity = registry.create();
    registry.emplace<Position>(machineEntity, pos);
    registry.emplace<MachineComponent>(machineEntity, info);
    
    // Create inventory with correct size
    InventoryContainer inventory(totalSlots);
    registry.emplace<InventoryContainer>(machineEntity, std::move(inventory));
    
    // ... rest of entity creation
}
```

### 5.5 Update CSV files with correct slot counts
**Location**: `src/data/registry/consumers.csv` and `src/data/registry/producers.csv`

**Implementation** (ensure CSV has slots_in and slots_out columns):
```csv
// consumers.csv example:
id,name,type,energy,slots_in,slots_out,capacity,tier,maxInput,maxOutput
36,heat_furnace,CONSUMER,HEAT,1,1,10000,0,32,32
48,heat_macerator,CONSUMER,HEAT,1,1,10000,0,32,32
52,steam_compressor,CONSUMER,STEAM,1,1,10000,0,32,32
// ... other machines
```

## Evidence Requirements
- [ ] defaultMachineSlotCount() uses MachineRegistry lookup
- [ ] MachineInfo contains slots_in and slots_out fields
- [ ] CSV files have correct slot count data
- [ ] All callers use updated slot count logic
- [ ] InventoryContainer created with correct size for each machine type

## Dependencies
- MachineRegistry must be fully implemented (Task 4 must be completed)
- CSV files must be updated with slot count data
- SimulationEngine must be updated to use new slot logic

## Testing
- Each machine type should have correct number of slots
- InventoryContainer should have correct capacity (1, 2, or 10 slots)
- Non-machine blocks should not receive inventory
- Slot count should match machine configuration in CSV

---