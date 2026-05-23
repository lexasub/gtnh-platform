# Task 4: Fix isMachineBlock() to Use MachineRegistry

## Objective
Replace hardcoded block ID checks in `isMachineBlock()` with lookup in `MachineRegistry` to ensure only registered machines receive ECS entities.

## Requirements

### 4.1 Locate and fix isMachineBlock() function
**Location**: `src/services/simulation_core/ECS/SimulationEngine.cpp` or `src/services/simulation_core/ECS/SimulationEngine.h`

**Current Implementation** (to be replaced):
```cpp
// Current hardcoded version (example):
bool isMachineBlock(uint16_t id) {
    return id == 36 || id == 48 || id == 52 || id == 14 || id == 35 || 
           id == 37 || id == 41 || id == 42 || id == 43 || // ... more hardcoded IDs
}
```

**Required Implementation**:
```cpp
// Updated to use MachineRegistry:
bool isMachineBlock(uint16_t id) {
    return MachineRegistry::getMachineInfo(id) != nullptr;
}
```

### 4.2 Verify MachineRegistry::getMachineInfo() method
**Location**: `src/services/simulation_core/ECS/MachineRegistry.h` / `.cpp`

**Implementation Check**:
```cpp
class MachineRegistry {
public:
    static const MachineInfo* getMachineInfo(uint16_t blockId);
    // ... other methods ...
};

struct MachineInfo {
    uint16_t id;
    std::string name;
    MachineType type; // CONSUMER/PRODUCER
    EnergyType energy;
    int slots_in;
    int slots_out;
    int capacity;
    int tier;
    int maxInput;
    int maxOutput;
};
```

### 4.3 Update all callers of isMachineBlock()
**Location**: `src/services/simulation_core/ECS/SimulationEngine.cpp`

**Implementation**:
```cpp
// Replace all isMachineBlock(id) checks with:
if (MachineRegistry::getMachineInfo(id) != nullptr) {
    // Create machine entity
}
```

### 4.4 Ensure MachineRegistry is properly initialized
**Location**: `src/services/simulation_core/ECS/MachineRegistry.cpp`

**Implementation**:
```cpp
// Load from CSV files during initialization:
void MachineRegistry::initialize() {
    // Load consumers.csv
    // Load producers.csv
    // Build lookup map
}
```

### 4.5 Update SimulationEngine::onBlockChanged()
**Location**: `src/services/simulation_core/ECS/SimulationEngine.cpp`

**Implementation**:
```cpp
void SimulationEngine::onBlockChanged(const BlockChangedEvent& event) {
    uint16_t blockId = event.blockId;
    
    // OLD (to be replaced):
    if (isMachineBlock(blockId)) {
        createMachineEntity(event.pos, blockId);
    }
    
    // NEW:
    const MachineInfo* machineInfo = MachineRegistry::getMachineInfo(blockId);
    if (machineInfo != nullptr) {
        createMachineEntity(event.pos, blockId, *machineInfo);
    }
}
```

## Evidence Requirements
- [ ] isMachineBlock() uses MachineRegistry::getMachineInfo()
- [ ] No hardcoded block IDs remain in isMachineBlock()
- [ ] MachineRegistry is properly initialized with all machine data
- [ ] All callers of isMachineBlock() updated to use new approach
- [ ] SimulationEngine creates entities only for registered machines

## Dependencies
- MachineRegistry must be fully implemented (consumers.csv + producers.csv loaded)
- MachineInfo struct must contain all required fields (slots_in, slots_out, etc.)

## Testing
- Unregistered machines (crafting_table, iron_pickaxe, chest, tools) should NOT receive ECS entities
- Registered machines should continue to work as before
- MachineRegistry lookup should return correct MachineInfo for all registered IDs

---