# Task 5: Pipe→Machine Insertion with Side Config

## Objective
Implement item delivery from pipe network into machine slots, respecting machine side_config roles (INPUT/OUTPUT) and slot availability.

## Requirements

### 5.1 Item insertion logic
**Location**: `src/services/pipe_network/PipeNetwork.cpp`

When item reaches destination pipe node adjacent to a machine:
1. Find machine block adjacent to the final pipe node
2. Check machine's `side_config[face]` — must be `INPUT` or `ANY`
3. Check machine inventory for available slots accepting this item_id
4. Insert item into first available slot
5. If insertion succeeds → remove from pipe buffer, publish delivered
6. If insertion fails → keep in pipe buffer (backpressure)

```cpp
struct InsertionResult {
    bool success;
    uint8_t slot_idx;       // which slot received the item
    std::string error;      // "slot_full", "wrong_side", "no_input_role"
};

InsertionResult PipeNetworkManager::insertIntoMachine(
    uint64_t machineNodeId,
    const ItemSlot& item,
    uint8_t incomingFace)
{
    InsertionResult result;
    
    // 1. Get machine component via SimulationCore (RPC or direct call)
    // 2. Check side_config[incomingFace] is INPUT or ANY
    // 3. Find first empty slot or matching item stack
    // 4. Insert item
    // 5. Return result
    return result;
}
```

### 5.2 Side config enforcement
**Location**: integration with MachineComponent (from Epic 4, Task B)

The machine's side_config determines which faces accept items:
- `INPUT` — items can enter from this face
- `OUTPUT` — items exit from this face (pipe can extract)
- `ENERGY` — energy connection only, no items
- `FLUID_IN`/`FLUID_OUT` — fluid items only
- `ANY` — auto (default, allows both directions)
- `NONE` — blocked

```cpp
bool canAcceptItem(uint8_t faceRole, ItemType type) {
    if (type == ITEM_NORMAL)
        return faceRole == FACE_INPUT || faceRole == FACE_ANY;
    if (type == ITEM_FLUID)
        return faceRole == FACE_FLUID_IN || faceRole == FACE_ANY;
    return false;
}
```

### 5.3 Machine inventory check
**Location**: integration with MachineSystem inventory

```cpp
// Check if machine has free slot for this item
// Called from PipeNetworkManager (or via callback)
bool hasSlotForItem(uint64_t machineNodeId, uint16_t itemId) {
    // 1. Get MachineComponent for this entity
    // 2. Get InventoryContainer
    // 3. Find slot where item_id matches (stackable) or slot is empty
    // 4. Return true if found
}
```

### 5.4 FlatBuffers integration — machine.item.insert RPC
**Location**: `src/protocol/pipe_network.fbs` or `src/protocol/simcore.fbs`

```flatbuffers
table MachineItemInsertReq {
    machine_pos: Vec3i;
    item: ItemStack;
    incoming_face: uint8;
}

table MachineItemInsertResp {
    success: bool;
    slot_idx: int8;          // -1 if failed
    remaining: ItemStack;    // what couldn't be inserted
}
```

### 5.5 SimulationCore handler
**Location**: `src/services/simulation_core/main.cpp` (or `MachineSystem.cpp`)

```cpp
// Subscribe to "machine.item.insert.request"
// Handle: find entity at position, check side_config, insert into inventory
// Return MachineItemInsertResp
```

## Integration Points
- `MachineComponent.side_config[]` — from Epic 4 (needs to exist first)
- `MachineSystem` — inventory access
- `PipeNetworkManager` — delivery routing

## Acceptance Criteria
- [ ] `insertIntoMachine()` checks side_config role before inserting
- [ ] Item is placed into correct machine slot
- [ ] Side config `NONE` blocks insertion
- [ ] Side config `INPUT` allows insertion
- [ ] Full inventory → backpressure (item stays in pipe)
- [ ] `MachineItemInsertReq/Resp` protocol messages exist
- [ ] SimulationCore handles machine.item.insert.request
- [ ] Backpressure: item stays in pipe buffer until slot free

## Dependencies
- Task 4 (item movement through pipe network)
- Epic 4 Task B (side_config in MachineComponent) — **hard dependency**
- MachineSystem inventory access

## Files to Modify
- `src/services/pipe_network/PipeNetwork.h/.cpp` — insertIntoMachine()
- `src/protocol/pipe_network.fbs` or `simcore.fbs` — MachineItemInsert messages
- `src/services/pipe_network/PipeNetworkService.cpp` — route to SimulationCore
- `src/services/simulation_core/main.cpp` — handle machine.item.insert.request
- `src/services/simulation_core/ECS/Systems/MachineSystem.cpp` — expose inventory API
