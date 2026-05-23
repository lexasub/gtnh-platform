# Task B1: side_config in MachineComponent

## Objective
Add a `side_config[6]` field to MachineComponent (or as a separate SideConfigComponent) that stores the role of each face of a machine block.

## Requirements

### 1.1 Side roles enum
**Location**: `src/protocol/core.fbs` or `ECS/Components/MachineComponent.h`

```cpp
enum class SideRole : uint8_t {
    INPUT       = 0,   // Items enter here
    OUTPUT      = 1,   // Items exit here
    ENERGY      = 2,   // Energy connection
    FLUID_IN    = 3,   // Fluids enter here
    FLUID_OUT   = 4,   // Fluids exit here
    ANY         = 5,   // Auto (default)
    NONE        = 6    // Blocked
};

// Face indices (GTNH convention)
// DOWN=0, UP=1, NORTH=2, SOUTH=3, WEST=4, EAST=5

// Default config: all faces AUTO
constexpr uint8_t DEFAULT_SIDE_CONFIG[6] = {5, 5, 5, 5, 5, 5};
```

### 1.2 Add side_config to MachineComponent
**Location**: `src/services/simulation_core/ECS/Components/MachineComponent.h`

```cpp
struct MachineComponent {
    uint16_t machine_id = 0;
    uint32_t mb_id = 0;
    uint32_t x = 0, y = 0, z = 0;
    uint64_t machine_instance_id = 0;
    bool managed_externally = false;
    
    // NEW:
    uint8_t side_config[6] = {5, 5, 5, 5, 5, 5};  // default: ANY
};
```

**Alternative (separate component):**
```cpp
struct SideConfig {
    uint8_t faces[6];        // 6 faces, each 0-6
};
```
Separate component is cleaner if not all machines need side_config. However, for simplicity, inline in MachineComponent is fine for MVP.

### 1.3 SideConfig helper functions
**Location**: `src/services/simulation_core/ECS/Components/SideConfig.h` (NEW)

```cpp
#pragma once
#include <cstdint>
#include <string>

// From SideRole enum
constexpr const char* sideRoleName(uint8_t role) {
    switch (role) {
        case 0: return "INPUT";
        case 1: return "OUTPUT";
        case 2: return "ENERGY";
        case 3: return "FLUID_IN";
        case 4: return "FLUID_OUT";
        case 5: return "ANY";
        case 6: return "NONE";
        default: return "UNKNOWN";
    }
}

// Cycle to next role (GTNH-style: skip non-applicable roles)
constexpr uint8_t nextSideRole(uint8_t current, bool hasFluid, bool hasEnergy) {
    switch (current) {
        case 0: return hasEnergy ? 2 : (hasFluid ? 3 : 1);  // INPUT → ENERGY (or FLUID_IN, or OUTPUT)
        case 1: return 0;  // OUTPUT → INPUT
        case 2: return 5;  // ENERGY → ANY
        case 3: return 4;  // FLUID_IN → FLUID_OUT
        case 4: return (hasFluid ? 3 : 0);  // FLUID_OUT → FLUID_IN or INPUT
        case 5: return 0;  // ANY → INPUT
        default: return 5;  // NONE → ANY
    }
}
```

### 1.4 Default role per machine type
When a machine is first placed, default roles based on MachineRegistry:
- Consumers: all faces = ANY (auto-detect input/output)
- Producers (generators): one face = ENERGY (output), rest = ANY
- Steam machines: one face = FLUID_OUT (steam output)
- Heat machines: one face = ENERGY (heat output)

## Acceptance Criteria
- [ ] `SideRole` enum with 7 roles (INPUT/OUTPUT/ENERGY/FLUID_IN/FLUID_OUT/ANY/NONE)
- [ ] `MachineComponent` has `side_config[6]` field
- [ ] Default config: all faces = ANY
- [ ] `nextSideRole()` cycles roles correctly
- [ ] `sideRoleName()` returns readable string
- [ ] No breaking changes to existing MachineComponent usage

## Dependencies
- None (component change)
- Required by: B2 (protocol), B4 (handler), B8 (PipeNetwork)

## Files to Create/Modify
- `src/services/simulation_core/ECS/Components/MachineComponent.h` — add side_config
- `src/services/simulation_core/ECS/Components/SideConfig.h` — NEW: helpers
