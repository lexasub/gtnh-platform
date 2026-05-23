# Task B4: Cyclic Role Switching (Server Handler)

## Objective
Implement the server-side handler for WRENCH_CYCLE: when a player uses a wrench on a machine face, cycle the side role (INPUT→OUTPUT→ENERGY→...) and respond with the new config.

## Requirements

### 4.1 WrenchHandler
**Location**: `src/services/simulation_core/Tools/WrenchHandler.h/.cpp` (NEW)

```cpp
#pragma once
#include <cstdint>
#include <unordered_map>
#include "ECS/Components/MachineComponent.h"
#include "ECS/Components/SideConfig.h"

struct WrenchCycleResult {
    bool success;
    std::string error;
    uint8_t newRole;
    uint8_t allRoles[6];
};

class WrenchHandler {
public:
    WrenchCycleResult cycleFace(uint64_t playerId, int32_t x, int32_t y, int32_t z, uint8_t face);
    
private:
    // Get the ECS entity at position
    entt::entity getEntityAt(int32_t x, int32_t y, int32_t z);
};
```

### 4.2 Cycle logic
```cpp
WrenchCycleResult WrenchHandler::cycleFace(uint64_t playerId, int32_t x, int32_t y, int32_t z, uint8_t face) {
    auto entity = getEntityAt(x, y, z);
    if (entity == entt::null) {
        return {false, "no_machine_at_position", 0, {}};
    }
    
    auto& machine = m_registry.get<MachineComponent>(entity);
    
    // Determine valid roles for this machine
    bool hasFluid = false;  // Check MachineRegistry for fluid capability
    bool hasEnergy = true;  // Most machines have energy
    
    // Cycle: current role → next role
    uint8_t currentRole = machine.side_config[face];
    uint8_t newRole = nextSideRole(currentRole, hasFluid, hasEnergy);
    
    // Apply
    machine.side_config[face] = newRole;
    
    // Publish update (see Task B6)
    publishConfigUpdated(x, y, z, machine.side_config, face, newRole);
    
    WrenchCycleResult result;
    result.success = true;
    result.newRole = newRole;
    std::copy(std::begin(machine.side_config), std::end(machine.side_config), result.allRoles);
    return result;
}
```

### 4.3 Cycle sequence per machine type

| Step | Consumer (no fluid) | Consumer (with fluid) | Generator |
|------|--------------------|----------------------|-----------|
| 0→1 | INPUT → OUTPUT | INPUT → OUTPUT | ENERGY → ANY |
| 1→2 | OUTPUT → ENERGY | OUTPUT → FLUID_IN | ANY → INPUT |
| 2→3 | ENERGY → ANY | FLUID_IN → FLUID_OUT | INPUT → OUTPUT |
| 3→4 | ANY → INPUT | FLUID_OUT → ENERGY | OUTPUT → ENERGY |
| 4→5 | — | ENERGY → ANY | ENERGY → ANY |
| 5→0 | — | ANY → INPUT | ANY → INPUT |

`nextSideRole()` from Task B1 handles this with `hasFluid`/`hasEnergy` flags.

### 4.4 Integration with ActionDispatcher
**Location**: `src/services/simulation_core/main.cpp`

```cpp
void ActionDispatcher::handleWrenchCycle(const Protocol::ToolAction* action) {
    auto result = m_wrenchHandler->cycleFace(
        action->player_id(),
        action->pos()->x(), action->pos()->y(), action->pos()->z(),
        action->face());
    
    // Send response
    auto resp = CreateMachineActionResp(builder, result.success, 
        result.error.c_str(), pos, action->face(), result.newRole, &rolesVector);
    // Publish to "client.machine.action.resp"
}

// Also handle MachineAction directly (from gateway):
void ActionDispatcher::handleMachineAction(const Protocol::MachineAction* action) {
    if (action->action() == Protocol::MachineActionType_WRENCH_CYCLE) {
        auto result = m_wrenchHandler->cycleFace(
            action->player_id(),
            action->pos()->x(), action->pos()->y(), action->pos()->z(),
            action->face());
        // ... send response ...
    }
}
```

### 4.5 Validation
- Only players holding a wrench (item_id=95) can cycle
- Only valid machine face indices (0-5)
- Only machines (not pipes/cables/blocks) respond to WRENCH_CYCLE
- No operation if role is already the last in cycle (wrap around)

## Acceptance Criteria
- [ ] WRENCH_CYCLE handler cycles face role: INPUT→OUTPUT→ENERGY→ANY→INPUT
- [ ] Role wraps: last valid role → first valid role
- [ ] Non-machine block → "no_machine_at_position" error
- [ ] Invalid face (6+) → "invalid_face" error
- [ ] Response includes all 6 face roles (for client sync)
- [ ] Role change triggers config update publish (→ Task B6)
- [ ] Only works when holding wrench item

## Dependencies
- Task B1 (side_config in MachineComponent)
- Task B2 (MachineAction protocol)
- Required by: B5 (persistence), B6 (publish), B7 (visual), B8 (PipeNetwork)

## Files to Create/Modify
- `src/services/simulation_core/Tools/WrenchHandler.h/.cpp` — NEW
- `src/services/simulation_core/main.cpp` — ActionDispatcher integration
