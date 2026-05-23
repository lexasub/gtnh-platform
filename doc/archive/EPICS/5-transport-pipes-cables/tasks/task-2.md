# Task 2: PipeNetwork — Item Graph (Separate from Energy)

## Objective
Add a separate item transport graph to PipeNetworkManager, parallel to the existing energy graph. Item pipes form their own connected components and use item buffers instead of energy buffers.

## Requirements

### 2.1 Extend PipeNode with item buffer
**Location**: `src/services/pipe_network/PipeNetwork.h`

**Current (from explore agent)**:
```cpp
// PipeNode — existing fields
struct PipeNode {
    uint64_t id;
    int32_t x, y, z;
    // Energy
    int32_t energyBuffer = 0;
    int32_t energyCapacity = 0;
    bool isSource = false;
    // Fluid
    int32_t fluidBuffer = 0;
    int32_t fluidCapacity = 0;
    uint16_t fluidId = 0;
    bool isFluidSource = false;
};
```

**Extend with**:
```cpp
struct ItemSlot {
    uint16_t item_id;
    uint8_t count;
};

struct PipeNode {
    // ... existing fields ...
    
    // Item transport (new)
    std::vector<ItemSlot> itemBuffer;  // items in transit
    uint8_t itemCapacity = 0;          // max items in buffer (1 for basic pipe, 4 for dense)
    bool isItemSource = false;
};
```

### 2.2 Add item graph membership tracking
**Location**: `src/services/pipe_network/PipeNetwork.h`

```cpp
struct PipeNetwork {
    std::vector<uint64_t> nodes;     // existing — energy
    std::vector<uint64_t> fluidNodes; // existing — fluid
    std::vector<uint64_t> itemNodes;  // NEW — item transport
    
    // ... existing energy fields ...
    float itemTransferRate = 1.0f;    // NEW: items/tick
};
```

### 2.3 Extend discoverNetwork() for item pipes
**Location**: `src/services/pipe_network/PipeNetwork.cpp`

**Current** (from agent):
```cpp
void PipeNetworkManager::discoverNetwork(uint64_t startNodeId) {
    // BFS from startNode — currently discovers energy connections
    // Uses node connections and builds PipeNetwork graph
}
```

**Change**: After discovering energy network, check node type:
- If node has `itemCapacity > 0` → add to `itemNodes` list
- Start a separate BFS traversal that only traverses item pipe edges

### 2.4 Add rebuildItemNetworks()
**Location**: `src/services/pipe_network/PipeNetwork.cpp`

```cpp
void PipeNetworkManager::rebuildItemNetworks() {
    // 1. Clear existing item networks
    // 2. For each item pipe node not yet assigned to a network:
    //    - BFS through item pipe edges only
    //    - Group into PipeNetwork with item-specific fields
    // 3. Store in m_itemNetworks (new member)
}
```

### 2.5 Add getItemNetwork() query
**Location**: `src/services/pipe_network/PipeNetwork.h`

```cpp
PipeNetwork* getItemNetwork(uint64_t nodeId);
// Returns the item network containing nodeId, or nullptr
```

### 2.6 Update PipeNetworkService to trigger item network rebuilds
**Location**: `src/services/pipe_network/PipeNetworkService.cpp`

When `world.blocks.changed` fires and the block is an item pipe (`isPipeBlock() == true`):
- Call `rebuildItemNetworks()`

## Architecture
```
Item network is SEPARATE from energy network:
- Same BFS algorithm, different edge type
- Item pipes connect only to other item pipes
- Machines connect to item pipes via side_config (INPUT/OUTPUT roles)
- Item network does NOT participate in energy distribution
```

## Acceptance Criteria
- [ ] `PipeNode` has `itemBuffer`, `itemCapacity`, `isItemSource` fields
- [ ] `PipeNetwork` has `itemNodes` vector + `itemTransferRate`
- [ ] `discoverNetwork()` correctly identifies item type nodes
- [ ] `rebuildItemNetworks()` creates separate networks for item pipes
- [ ] Item pipe blocks trigger network rebuild on placement/removal
- [ ] No interference with existing energy/fluid graph

## Dependencies
- Task 1 (pipe block IDs + isPipeBlock())
- Required by: Task 4 (item movement), Task 5 (pipe→machine)

## Files to Modify
- `src/services/pipe_network/PipeNetwork.h` — PipeNode + PipeNetwork extensions
- `src/services/pipe_network/PipeNetwork.cpp` — discoverNetwork, rebuildItemNetworks
- `src/services/pipe_network/PipeNetworkService.cpp` — trigger rebuilds
- `src/services/pipe_network/PipeNetworkService.h` — may need new method declarations
