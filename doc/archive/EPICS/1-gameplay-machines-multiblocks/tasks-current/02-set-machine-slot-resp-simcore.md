# Task 2: Add SetMachineSlotResp Processing in SimulationCore

## Objective
Implement SetMachineSlotResp generation in SimulationCore for machine slot operations, providing explicit acknowledgment to the client.

## Requirements

### 2.1 Update SimulationCore main.cpp handler
**Location**: `src/services/simulation_core/main.cpp` (lines 382-436)

**Current Implementation**:
```cpp
void onPlayerMachineSlot(const PlayerAction& action, 
                         const SetMachineSlotReq& req) {
    // ... existing validation and processing ...
    
    // Save to EntityStateStore (line ~420)
    entityStateStore->save(entityId, state);
    
    // Publish BlockEntityUpdate to client (line ~430)
    publishBlockEntityUpdate(machinePos, state);
    
    // TODO: Send SetMachineSlotResp to client
}
```

**Required Implementation**:
```cpp
void onPlayerMachineSlot(const PlayerAction& action, 
                         const SetMachineSlotReq& req) {
    // ... existing validation and processing ...
    
    // Save to EntityStateStore
    entityStateStore->save(entityId, state);
    
    // Publish BlockEntityUpdate to client
    publishBlockEntityUpdate(machinePos, state);
    
    // Create and send SetMachineSlotResp
    SetMachineSlotResp resp;
    resp.pos = req.pos;
    resp.slot_idx = req.slot_idx;
    resp.success = true;
    resp.error = "";
    resp.item = state.inventory.getSlot(req.slot_idx);
    
    // Send response through Gateway
    gateway->sendResponse(action.playerId, resp);
}
```

**Error Handling**:
```cpp
// If operation fails:
SetMachineSlotResp resp;
resp.pos = req.pos;
resp.slot_idx = req.slot_idx;
resp.success = false;
resp.error = getErrorMessage(); // "inventory_full", "no_permission", etc.
resp.item = state.inventory.getSlot(req.slot_idx);

gateway->sendResponse(action.playerId, resp);
```

### 2.2 Add SetMachineSlotResp struct definition
**Location**: `src/services/simulation_core/ECS/Protocol.h` or similar

**Implementation**:
```cpp
struct SetMachineSlotResp {
    Vec3i pos;
    uint8_t slot_idx;
    bool success;
    std::string error;
    ItemStack item;  // Current item in slot after operation
};
```

### 2.3 Update Gateway integration
**Location**: `src/services/simulation_core/main.cpp`

**Implementation**:
```cpp
// Include gateway header
#include "gateway/Gateway.h"

// In onPlayerMachineSlot:
gateway->sendResponse(playerId, resp);
```

## Error Cases

### 2.4 Inventory Full
- Check if slot can accept item
- Set `success = false`, `error = "inventory_full"`
- `item` should contain current slot state

### 2.5 No Permission
- Verify player has access to machine
- Set `success = false`, `error = "no_permission"`
- `item` should contain current slot state

### 2.6 Invalid Slot Index
- Check if slot_idx < inventory capacity
- Set `success = false`, `error = "invalid_slot"`
- `item` should contain current slot state

## Evidence Requirements
- [ ] SimulationCore handler creates SetMachineSlotResp
- [ ] Response contains correct position, slot_idx, success flag
- [ ] Error cases populate error field correctly
- [ ] Item field reflects current slot state after operation
- [ ] Gateway integration works (no compilation errors)

## Dependencies
- Gateway integration requires Gateway.h included
- FlatBuffers protocol changes (Task 1 must be completed first)

## Testing
- Client should receive success response for valid operations
- Error responses should contain appropriate error messages
- UI should update based on response content

---