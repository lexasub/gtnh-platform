# Task 1: Add SetMachineSlotResp to core.fbs

## Objective
Add `SetMachineSlotResp` FlatBuffers table to `core.fbs` to provide explicit acknowledgment for machine slot operations.

## Requirements

### 1.1 Add SetMachineSlotResp table definition
**Location**: `src/protocol/core.fbs` (lines 385-398)

**Implementation**:
```flatbuffers
table SetMachineSlotResp {
    pos: Vec3i;
    slot_idx: uint8;
    success: bool;
    error: string;       // "inventory_full", "no_permission", etc.
    item: ItemStack;     // текущий предмет в слоте после операции
}
```

**Details**:
- `pos`: Position of the machine block (Vec3i)
- `slot_idx`: Index of the slot that was modified (0-8)
- `success`: Operation result (true/false)
- `error`: Error message if operation failed, empty string if successful
- `item`: Current item stack in the slot after the operation

### 1.2 Update GatewayMsg enum
**Location**: `src/services/gateway/gateway.h` / `src/services/gateway/NetClient.h`

**Implementation**:
- Add `kSetMachineSlotResp = 16` to `GatewayMsg` enum
- This allows Gateway to distinguish between requests and responses

### 1.3 Add SetMachineSlotResp serialization in Gateway
**Location**: `src/services/gateway/gateway.cpp` (around lines 358-363)

**Implementation**:
```cpp
// Add response serialization:
if (msg.type == GatewayMsg::kSetMachineSlotResp) {
    auto& resp = std::get<SetMachineSlotResp>(msg.data);
    // Serialize response to client
}
```

### 1.4 Add SetMachineSlotResp deserialization in NetClient
**Location**: `src/services/gateway/NetClient.h` / `.cpp`

**Implementation**:
```cpp
// Add response handling:
if (msg.type == GatewayMsg::kSetMachineSlotResp) {
    auto& resp = std::get<SetMachineSlotResp>(msg.data);
    // Process response
}
```

## Evidence Requirements
- [ ] core.fbs contains SetMachineSlotResp table definition
- [ ] GatewayMsg enum includes kSetMachineSlotResp
- [ ] Gateway can serialize/deserialize SetMachineSlotResp
- [ ] NetClient can handle SetMachineSlotResp messages

## Dependencies
- core.fbs must be regenerated after changes
- All services using core.fbs must be recompiled

## Testing
- Client should receive SetMachineSlotResp for every slot operation
- Error cases should populate error field correctly
- Successful operations should populate item field with current state

---