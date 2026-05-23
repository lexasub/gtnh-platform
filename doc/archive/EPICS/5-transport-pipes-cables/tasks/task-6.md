# Task 6: Item Buffer Persistence via EntityStateStore

## Objective
Save and restore item pipe buffers (items in transit) through EntityStateStore (ESS) so items survive service restarts.

## Requirements

### 6.1 Define item buffer FlatBuffers schema
**Location**: `src/protocol/entity_state_store.fbs` (or `pipe_network.fbs`)

```flatbuffers
table ItemBufferSlot {
    item_id: uint16;
    count: uint8;
}

table ItemBufferData {
    node_id: uint64;
    slots: [ItemBufferSlot];
    active_routes: [ActiveRoute];  // items in transit between nodes
}

table ActiveRoute {
    route_id: uint64;
    from_node: uint64;
    to_node: uint64;
    item: ItemBufferSlot;
    current_hop: uint16;
    total_hops: uint16;
}
```

### 6.2 Serialize item buffers to ESS
**Location**: `src/services/pipe_network/PipeNetwork.cpp`

```cpp
class PipeNetworkManager {
    // NEW:
    std::vector<uint8_t> serializeItemBuffers();
    void deserializeItemBuffers(const std::vector<uint8_t>& data);
    
    // Called on:
    void onSave();    // before shutdown or periodically
    void onLoad();    // on startup
};
```

### 6.3 Save on service shutdown / periodic save
**Location**: `src/services/pipe_network/PipeNetworkService.cpp`

```cpp
void PipeNetworkService::onShutdown() {
    auto data = m_manager->serializeItemBuffers();
    
    // Save via ESS RPC
    EntityStateSaveReq req;
    req.key = "pipe_network_item_buffers";
    req.data = data;
    m_essClient->save(req);
}

void PipeNetworkService::onStartup() {
    // Load from ESS
    auto resp = m_essClient->load("pipe_network_item_buffers");
    if (resp.found) {
        m_manager->deserializeItemBuffers(resp.data);
    }
}
```

### 6.4 Periodic autosave (optional)
**Location**: `src/services/pipe_network/PipeNetworkService.cpp`

```cpp
// Save every 30 seconds to prevent data loss on crash
void PipeNetworkService::onTick() {
    m_autosaveTimer += deltaTime;
    if (m_autosaveTimer >= 30.0f) {
        m_autosaveTimer = 0.0f;
        onSave();
    }
    
    // ... existing tick logic ...
    m_manager->advanceItems();
    m_manager->distributeEnergy();
}
```

### 6.5 Items in transit — restore active routes
On load:
1. Restore item buffers for each pipe node
2. Restore active routes (items between nodes)
3. Items at `current_hop` resume from that position

## Acceptance Criteria
- [ ] `ItemBufferData` FlatBuffers schema defined
- [ ] `ItemBufferSlot` and `ActiveRoute` tables defined
- [ ] `serializeItemBuffers()` produces valid FlatBuffer binary
- [ ] `deserializeItemBuffers()` restores exact buffer state
- [ ] Save triggered on shutdown (and optionally on timer)
- [ ] Load triggered on startup
- [ ] ESS RPC integration (save/load by key)
- [ ] Items in transit survive restart (resume from current_hop)
- [ ] No data loss on clean shutdown

## Dependencies
- Task 3 (PushItemToPipe protocol — item buffer structures)
- Task 4 (item movement — active routes)
- EntityStateStore service (already exists)

## Files to Modify
- `src/protocol/entity_state_store.fbs` — ItemBufferData tables
- `src/services/pipe_network/PipeNetwork.h/.cpp` — serialization methods
- `src/services/pipe_network/PipeNetworkService.h/.cpp` — save/load on lifecycle
