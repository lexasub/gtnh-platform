# Task 6: Implement EnergyStorage Initialization from MachineRegistry

## Objective
Replace hardcoded EnergyStorage values with dynamic lookup from MachineRegistry based on machine configuration.

## Requirements

### 6.1 Update MachineRegistry to include EnergyStorage fields
**Location**: `src/services/simulation_core/ECS/MachineRegistry.h`

**Implementation** (ensure MachineInfo has all required EnergyStorage fields):
```cpp
struct MachineInfo {
    uint16_t id;
    std::string name;
    MachineType type;
    EnergyType energy;          // Add this field
    int slots_in;
    int slots_out;
    int capacity;              // Add this field
    int tier;                  // Add this field
    int maxInput;              // Add this field
    int maxOutput;             // Add this field
};
```

### 6.2 Update MachineInfo initialization to load from CSV
**Location**: `src/services/simulation_core/ECS/MachineRegistry.cpp`

**Implementation**:
```cpp
void MachineRegistry::loadFromCSV(const std::string& consumersPath, 
                                  const std::string& producersPath) {
    // Load consumers.csv
    std::ifstream consumersFile(consumersPath);
    std::string line;
    while (std::getline(consumersFile, line)) {
        auto fields = splitCSVLine(line);
        if (fields.size() >= 10) {  // id,name,type,energy,slots_in,slots_out,capacity,tier,maxInput,maxOutput
            MachineInfo info;
            info.id = std::stoi(fields[0]);
            info.name = fields[1];
            info.type = stringToMachineType(fields[2]);
            info.energy = stringToEnergyType(fields[3]);
            info.slots_in = std::stoi(fields[4]);
            info.slots_out = std::stoi(fields[5]);
            info.capacity = std::stoi(fields[6]);
            info.tier = std::stoi(fields[7]);
            info.maxInput = std::stoi(fields[8]);
            info.maxOutput = std::stoi(fields[9]);
            
            registry[info.id] = info;
        }
    }
    
    // Load producers.csv (similar)
}
```

### 6.3 Update SimulationEngine::createMachineEntity() with EnergyStorage
**Location**: `src/services/simulation_core/ECS/SimulationEngine.cpp`

**Implementation**:
```cpp
void SimulationEngine::createMachineEntity(const Vec3i& pos, 
                                          uint16_t blockId,
                                          const MachineInfo& info) {
    entt::entity machineEntity = registry.create();
    registry.emplace<Position>(machineEntity, pos);
    registry.emplace<MachineComponent>(machineEntity, info);
    
    // OLD hardcoded EnergyStorage:
    // EnergyStorage energy(info.capacity, info.tier, info.maxInput, info.maxOutput);
    
    // NEW EnergyStorage from MachineInfo:
    EnergyStorage energyStorage(
        info.capacity,           // capacity
        info.tier,               // tier
        info.maxInput,           // maxInput
        info.maxOutput           // maxOutput
    );
    registry.emplace<EnergyStorage>(machineEntity, std::move(energyStorage));
    
    // Create inventory with correct slot count
    InventoryContainer inventory(info.slots_in + info.slots_out);
    registry.emplace<InventoryContainer>(machineEntity, std::move(inventory));
    
    // ... rest of entity creation
}
```

### 6.4 Update EnergyStorage struct to include all fields
**Location**: `src/services/simulation_core/ECS/Components/EnergyStorage.h`

**Implementation**:
```cpp
struct EnergyStorage {
    int capacity;      // Total energy storage capacity
    int current;       // Current energy level
    int tier;          // Energy tier (0=basic, 1=advanced, etc.)
    int maxInput;      // Maximum energy input rate
    int maxOutput;     // Maximum energy output rate
    
    EnergyStorage(int cap, int t, int maxIn, int maxOut)
        : capacity(cap), current(0), tier(t), maxInput(maxIn), maxOutput(maxOut) {}
};
```

### 6.5 Add fallback EnergyStorage for non-registered machines
**Location**: `src/services/simulation_core/ECS/SimulationEngine.cpp`

**Implementation** (for machines that shouldn't exist but might):
```cpp
// If machine is not in registry but still needs EnergyStorage (shouldn't happen after Task 4):
if (info == nullptr) {
    // This should not occur after Task 4 fixes isMachineBlock()
    // But keep fallback for safety:
    EnergyStorage fallback(10000, 0, 32, 32);  // Default values
    registry.emplace<EnergyStorage>(machineEntity, std::move(fallback));
}
```

## Evidence Requirements
- [ ] MachineInfo contains all EnergyStorage fields (capacity, tier, maxInput, maxOutput)
- [ ] CSV files have correct EnergyStorage data
- [ ] EnergyStorage is initialized with MachineInfo values
- [ ] EnergyStorage struct has all required fields
- [ ] Fallback EnergyStorage exists for safety

## Dependencies
- MachineRegistry must be fully implemented (Task 4 & 5)
- CSV files must be updated with EnergyStorage data
- EnergyStorage struct must be updated
- SimulationEngine must use new EnergyStorage initialization

## Testing
- Each machine should have correct capacity (10000, 20000, etc.)
- Each machine should have correct tier (0 for basic, 1 for advanced)
- Input/output rates should match machine type
- EnergyStorage should work correctly with PipeNetwork

---