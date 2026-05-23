# Task A2: ToolAction FlatBuffers Protocol

## Objective
Add ToolAction union type and related tables to core.fbs and gateway.fbs for electric tool actions (mine block with drill, use wrench, charge item, etc.).

## Requirements

### 2.1 Add ToolActionType enum to core.fbs
**Location**: `src/protocol/core.fbs` (near existing PlayerActionType)

```flatbuffers
enum ToolActionType : uint8 {
    MINE_BLOCK = 0,        // Drill/chainsaw: break block with energy cost
    WRENCH_CYCLE = 1,      // Wrench: cycle side role on machine
    CHARGE_ITEM = 2,       // Battery buffer: charge tool in slot
    TOOL_INFO = 3,         // Query tool status (charge, tier)
}
```

### 2.2 Add ToolAction table
**Location**: `src/protocol/core.fbs`

```flatbuffers
table ToolAction {
    player_id: uint64;
    action: ToolActionType;
    pos: Vec3i;              // Target block position
    face: uint8;             // Face being interacted with (0-5)
    item_id: uint16;         // Tool item ID being used
    slot_idx: uint8;         // Inventory slot of the tool
    extra_data: uint16;      // Action-specific data (e.g., target slot for charging)
}
```

### 2.3 Add ToolActionResp
**Location**: `src/protocol/core.fbs`

```flatbuffers
table ToolActionResp {
    success: bool;
    error: string;            // "no_energy", "wrong_tier", "no_tool", etc.
    energy_remaining: uint32; // Tool charge after action
    mined_block_id: uint16;   // If MINE_BLOCK: what was mined
    new_side_role: uint8;     // If WRENCH_CYCLE: new role (0-6)
}
```

### 2.4 Update GatewayMsg union
**Location**: `src/protocol/gateway.fbs` (or `core.fbs` — wherever GatewayPayload is defined)

Add to the GatewayPayload union:
```flatbuffers
union GatewayPayload {
    // ... existing types ...
    ToolAction,              // NEW
    ToolActionResp,          // NEW
}
```

### 2.5 Add GatewayMsg type
**Location**: `src/protocol/gateway.fbs`

```flatbuffers
table GatewayMsg {
    payload: GatewayPayload;
    // ... existing fields ...
}
```

Add `kToolAction` and `kToolActionResp` to the message type enum.

## Acceptance Criteria
- [ ] `ToolActionType` enum with 4 action types
- [ ] `ToolAction` table with all required fields
- [ ] `ToolActionResp` table with result fields
- [ ] GatewayPayload union includes ToolAction + ToolActionResp
- [ ] GatewayMsg enum includes kToolAction + kToolActionResp
- [ ] No breaking changes to existing protocol
- [ ] FlatBuffers compiles without errors

## Dependencies
- Task A1 (tool item IDs)
- Required by: Task A3 (ActionDispatcher), Task B2 (wrench protocol)

## Files to Modify
- `src/protocol/core.fbs` — ToolActionType, ToolAction, ToolActionResp
- `src/protocol/gateway.fbs` — GatewayPayload union extension
