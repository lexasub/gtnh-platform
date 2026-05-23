# Task 7: Add Missing Machines to MachineRegistry

## Objective
Add all machine block IDs from items.csv to MachineRegistry (consumers.csv + producers.csv) with correct machine parameters.

## Requirements

### 7.1 Analyze items.csv for machine block IDs
**Location**: `src/data/registry/items.csv`

**Implementation**:
```bash
# Find block IDs that should be machines:
grep -E ",(furnace|macerator|generator|boiler|compressor|extractor|mixer|alloy_smelter)," items.csv
# or manually inspect for machine-like items
```

**Expected machine block IDs** (from current registry):
- 36: heat_furnace (CONSUMER, HEAT)
- 46: heat_generator (PRODUCER, HEAT) 
- 48: heat_macerator (CONSUMER, HEAT)
- 49: steam_solid_boiler (PRODUCER, STEAM)
- 50: steam_heat_boiler (PRODUCER, STEAM/HEAT)
- 51: steam_macerator (CONSUMER, STEAM)
- 52: steam_compressor (CONSUMER, STEAM)
- 60: bronze_alloy_smelter (CONSUMER, STEAM)
- 61: steam_extractor (CONSUMER, STEAM)
- 62: steam_mixer (CONSUMER, STEAM)
- 63: creative_generator (PRODUCER, ELECTRICITY)

### 7.2 Verify consumers.csv has correct machines
**Location**: `src/data/registry/consumers.csv`

**Текущие CONSUMER машины**:
```csv
id,name,type,energy,slots_in,slots_out,capacity,tier,maxInput,maxOutput
36,heat_furnace,CONSUMER,HEAT,1,1,10000,0,32,32
48,heat_macerator,CONSUMER,HEAT,1,1,10000,0,32,32
51,steam_macerator,CONSUMER,STEAM,1,1,15000,0,32,64
52,steam_compressor,CONSUMER,STEAM,1,1,18000,1,32,64
60,bronze_alloy_smelter,CONSUMER,STEAM,2,1,20000,1,64,32
61,steam_extractor,CONSUMER,STEAM,1,1,15000,1,48,24
62,steam_mixer,CONSUMER,STEAM,2,0,18000,1,64,0
```

### 7.3 Verify producers.csv has correct machines
**Location**: `src/data/registry/producers.csv`

**Текущие PRODUCER машины**:
```csv
id,name,type,energy,slots_in,slots_out,capacity,tier,maxInput,maxOutput
46,heat_generator,PRODUCER,HEAT,0,1,15000,0,0,64
49,steam_solid_boiler,PRODUCER,STEAM,2,1,25000,1,64,32
50,steam_heat_boiler,PRODUCER,STEAM/HEAT,1,1,20000,1,32,32
63,creative_generator,PRODUCER,ELECTRICITY,10,0,100000,5,0,1000
```

### 7.4 Verify all machine types are represented
**Implementation**:
```cpp
// In MachineRegistry::initialize():
void MachineRegistry::verifyCompleteness() {
    // Check that all expected machines are present
    std::vector<uint16_t> expectedMachines = {
        36, 46, 48, 49, 50, 51, 52, 60, 61, 62, 63
    };
    
    for (uint16_t id : expectedMachines) {
        if (getMachineInfo(id) == nullptr) {
            logError("Machine " + std::to_string(id) + " missing from registry");
        }
    }
}
```

### 7.5 Update MachineRegistry documentation
**Location**: `src/services/simulation_core/ECS/MachineRegistry.h`

**Implementation** (add comments):
```cpp
// MachineRegistry contains all machine definitions loaded from CSV files.
// Each machine has unique ID, type (CONSUMER/PRODUCER), energy type,
// slot counts, energy capacity, tier, and input/output limits.
// 
// consumers.csv: Machines that consume energy to operate
// producers.csv: Machines that generate energy
// All IDs must correspond to actual machine block IDs in the game.
```

### 7.6 Update CSV validation
**Location**: `src/services/simulation_core/ECS/MachineRegistry.cpp`

**Implementation**:
```cpp
void MachineRegistry::validateCSV() {
    // Validate each machine entry
    for (const auto& [id, info] : registry) {
        // Check required fields
        assert(info.slots_in >= 0 && info.slots_out >= 0);
        assert(info.capacity > 0);
        assert(info.tier >= 0 && info.tier <= 5);
        assert(info.maxInput > 0 && info.maxOutput > 0);
        
        // Log machine info for debugging
        logDebug("Machine " + std::to_string(id) + ": " + 
                info.name + " - " + 
                std::to_string(info.slots_in + info.slots_out) + " slots, " +
                std::to_string(info.capacity) + " capacity, tier " + 
                std::to_string(info.tier));
    }
}
```

## Evidence Requirements
- [ ] All expected machines are present in consumers.csv
- [ ] All expected machines are present in producers.csv
- [ ] CSV files have correct machine parameters
- [ ] MachineRegistry can load all machines from CSV
- [ ] Validation passes without errors
- [ ] Each machine has correct slots, capacity, tier, and input/output limits

## Dependencies
- MachineRegistry implementation (Task 4)
- File parsing utilities for CSV
- Validation logging system

## Testing
- Each machine ID should resolve to correct MachineInfo
- Slot counts should match machine type (1, 2, or 10)
- Capacity values should be reasonable (10000-100000)
- Tier values should be appropriate (0-5)
- Input/output limits should be balanced

---