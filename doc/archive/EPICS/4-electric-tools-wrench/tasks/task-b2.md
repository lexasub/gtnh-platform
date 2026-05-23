# Task B2: MachineAction SET_SIDE_CONFIG Protocol

## Objective
Add MachineAction FlatBuffers protocol for wrench-based side config changes: client sends WRENCH_CYCLE, server responds with updated side role.

## Requirements

### 2.1 MachineActionType enum
**Location**: `src/protocol/core.fbs`

```flatbuffers
enum MachineActionType : uint8 {
    WRENCH_CYCLE = 0,       // Cycle side role on a face
    SET_SIDE_CONFIG = 1,    // Directly set side config (future: GUI)
    CONFIG_UPDATED = 2,     // Server → Client: config changed
}
```

### 2.2 MachineAction table
```flatbuffers
table MachineAction {
    player_id: uint64;
    action: MachineActionType;
    pos: Vec3i;              // Machine position
    face: uint8;             // Face being interacted with (0-5)
    new_role: uint8;         // For SET_SIDE_CONFIG: desired role
}
```

### 2.3 MachineActionResp table
```flatbuffers
table MachineActionResp {
    success: bool;
    error: string;
    pos: Vec3i;
    face: uint8;
    new_role: uint8;          // Role after change
    roles: [uint8];           // All 6 face roles (for client sync)
}
```

### 2.4 Update GatewayMsg / GatewayPayload
**Location**: `src/protocol/gateway.fbs`

```flatbuffers
// Add to GatewayPayload union:
union GatewayPayload {
    // ... existing ...
    MachineAction,
    MachineActionResp,
}

// Add to GatewayMsg type enum:
// kMachineAction, kMachineActionResp
```

### 2.5 Update core.fbs GatewayMsgType
**Location**: `src/protocol/core.fbs`

Add to the message type enum:
```flatbuffers
kMachineAction = 12,
kMachineActionResp = 13,
```

### 2.6 Gateway routing
**Location**: `src/services/gateway/`

```cpp
// Route MachineAction from client to SimulationCore
if (msg->payload_type() == GatewayPayload_MachineAction) {
    routerClient->publish("simcore.machine.action", data);
}

// Route MachineActionResp from SimulationCore to client
if (topic == "client.machine.action.resp") {
    sendToClient(data);
}
```

## Acceptance Criteria
- [ ] `MachineActionType` enum with WRENCH_CYCLE, SET_SIDE_CONFIG, CONFIG_UPDATED
- [ ] `MachineAction` table with player_id, action, pos, face, new_role
- [ ] `MachineActionResp` table with success, error, new_role, roles[6]
- [ ] GatewayPayload union includes both MachineAction and MachineActionResp
- [ ] GatewayMsg type enum includes kMachineAction/kMachineActionResp
- [ ] Gateway routes MachineAction ↔ SimulationCore
- [ ] No breaking changes to existing protocol

## Dependencies
- Task B1 (side_config — defines roles)
- Required by: B3 (wrench client), B4 (handler)

## Files to Modify
- `src/protocol/core.fbs` — MachineActionType, MachineAction, MachineActionResp
- `src/protocol/gateway.fbs` — GatewayPayload union, GatewayMsg type
- `src/services/gateway/` — message routing
