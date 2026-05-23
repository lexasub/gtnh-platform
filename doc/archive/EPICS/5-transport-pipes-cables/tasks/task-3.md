# Task 3: PushItemToPipe Protocol + FlatBuffers

## Objective
Add FlatBuffers protocol messages for item transport: machine pushes item to connected pipe, pipe network routes item, destination machine receives item.

## Requirements

### 3.1 Add item transport messages to pipe_network.fbs
**Location**: `src/protocol/pipe_network.fbs`

**Current (from explore agent)** — only energy/fluid messages exist:
```flatbuffers
// Existing topics:
// energy.node.update, energy.check.request/response
// energy.consume.request/response, energy.flow
```

**Add**:
```flatbuffers
table PushItemReq {
    source_pos: Vec3i;       // machine position pushing item
    target_network_id: uint64; // item network ID
    item: ItemStack;          // what to push (item_id + count)
    source_face: uint8;       // which face the item exits from (0-5)
}

table PushItemResp {
    success: bool;
    error: string;            // "network_full", "no_route", etc.
    item: ItemStack;          // what was actually pushed (could be partial)
}

table ItemFlowEvent {
    timestamp: uint64;
    network_id: uint64;
    from_pos: Vec3i;
    to_pos: Vec3i;
    item: ItemStack;
    status: ItemFlowStatus;  // IN_TRANSIT, DELIVERED, LOST
}

enum ItemFlowStatus : byte {
    IN_TRANSIT = 0,
    DELIVERED = 1,
    LOST = 2
}
```

### 3.2 Update PipeNetworkManager API
**Location**: `src/services/pipe_network/PipeNetwork.h`

```cpp
struct PushItemResult {
    bool success;
    std::string error;
    ItemSlot remaining;  // what couldn't be pushed
};

class PipeNetworkManager {
    // ... existing methods ...
    
    // NEW:
    PushItemResult pushItem(uint64_t sourceNodeId, const ItemSlot& item, uint8_t sourceFace);
    bool canDeliverToMachine(uint64_t targetNodeId, uint16_t itemId, uint8_t count);
    uint64_t findItemRoute(uint64_t sourceNodeId, uint64_t targetNodeId);
};
```

### 3.3 Add item transport method to PipeEnergyClient (or new PipeItemClient)
**Location**: `src/services/simulation_core/Network/PipeItemClient.h` (NEW) or extend `PipeEnergyClient`

```cpp
class PipeItemClient {
public:
    PipeItemClient(const std::string& routerHost, uint16_t routerPort);
    
    // Push item from machine to pipe network
    bool pushItem(int32_t x, int32_t y, int32_t z, uint16_t itemId, uint8_t count, uint8_t face);
    
    // Register machine as item receiver
    void registerItemReceiver(int32_t x, int32_t y, int32_t z, uint8_t faceRole);
    
private:
    MessageRouterClient* m_router;
};
```

### 3.4 Add item transport topic to PipeNetworkService
**Location**: `src/services/pipe_network/PipeNetworkService.cpp`

**Add subscriptions**:
```cpp
// In PipeNetworkService::Start():
m_router->subscribe("item.push.request", [this](const Message& msg) {
    handlePushItemRequest(msg);
});
m_router->subscribe("item.push.response", [this](const Message& msg) {
    handlePushItemResponse(msg);
});
```

## Wire Protocol
```
Machine (SimCore) → "item.push.request" → PipeNetwork
PipeNetwork → BFS route → "item.push.response" → Machine
ItemFlowEvent → "item.flow" → all subscribers (debug/monitoring)
```

## Acceptance Criteria
- [ ] `PushItemReq` / `PushItemResp` / `ItemFlowEvent` tables in pipe_network.fbs
- [ ] `ItemFlowStatus` enum defined
- [ ] `PipeNetworkManager::pushItem()` declared and implemented (can stub return false initially)
- [ ] `findItemRoute()` for route lookup (return 0 = no route)
- [ ] PipeNetworkService subscribes to `item.push.request`
- [ ] `PipeItemClient` class created with `pushItem()` method
- [ ] No compilation errors

## Dependencies
- Task 1 (pipe block IDs)
- Task 2 (item graph structure)
- Required by: Task 4 (item movement), Task 5 (pipe→machine)

## Files to Modify
- `src/protocol/pipe_network.fbs` — new tables + enum
- `src/services/pipe_network/PipeNetwork.h` — PushItemResult, new methods
- `src/services/pipe_network/PipeNetwork.cpp` — pushItem() stub
- `src/services/pipe_network/PipeNetworkService.h/.cpp` — topic subscriptions
- `src/services/simulation_core/Network/PipeItemClient.h/.cpp` — NEW: client class
