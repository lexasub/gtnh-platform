# Task 9: Fluid Graph — Expand PipeNetwork distributeFluid()

## Objective
Activate the existing `distributeFluid()` in PipeNetworkManager and connect it to fluid_pipe blocks. Fluids flow through a separate graph (parallel to energy) using fluid item IDs.

## Requirements

### 9.1 Review and fix existing fluid infrastructure
**Location**: `src/services/pipe_network/PipeNetwork.h/.cpp`

**Current (from explore agent)**:
```cpp
// PipeNode already has:
int32_t fluidBuffer = 0;
int32_t fluidCapacity = 0;
uint16_t fluidId = 0;
bool isFluidSource = false;

// PipeNetwork already has:
std::vector<uint64_t> fluidNodes;

// PipeNetworkManager already has:
void distributeFluid(uint64_t networkId);
```

**Issues to fix**:
1. `fluidId` is `uint16_t` — should match item_id (1000+)
2. `fluidBuffer` is `int32_t` — should be item_count (uint8_t based on stack size)
3. `distributeFluid()` may distribute to all sinks equally — need to verify algorithm

### 9.2 Update PipeNode fluid buffer for items-as-fluids
```cpp
// Change to match item transport model:
struct PipeNode {
    // ... existing energy fields ...
    
    // Fluid (updated for items-as-fluids)
    std::vector<ItemSlot> fluidBuffer;  // fluid items in transit (reuse ItemSlot)
    uint8_t fluidCapacity = 1;          // fluid pipe capacity
    bool isFluidSource = false;
    uint16_t fluidItemId = 0;           // which fluid type this pipe handles
};
```

### 9.3 Activate fluid network discovery
**Location**: `src/services/pipe_network/PipeNetwork.cpp`

```cpp
void PipeNetworkManager::rebuildFluidNetworks() {
    m_fluidNetworks.clear();
    std::unordered_set<uint64_t> visited;
    
    for (auto& [id, node] : m_nodes) {
        if (!node.isFluidNode() || visited.count(id)) continue;
        
        // BFS through fluid pipe edges only
        PipeNetwork network;
        std::queue<uint64_t> queue;
        queue.push(id);
        visited.insert(id);
        
        while (!queue.empty()) {
            uint64_t current = queue.front(); queue.pop();
            network.fluidNodes.push_back(current);
            
            for (const auto& edge : m_edges) {
                uint64_t neighbor = getNeighbor(current, edge);
                if (neighbor && isFluidEdge(edge) && !visited.count(neighbor)) {
                    visited.insert(neighbor);
                    queue.push(neighbor);
                }
            }
        }
        
        m_fluidNetworks.push_back(network);
    }
}
```

### 9.4 Implement distributeFluid() for items-as-fluids
**Location**: `src/services/pipe_network/PipeNetwork.cpp`

```cpp
void PipeNetworkManager::distributeFluid(uint64_t networkId) {
    auto* network = getFluidNetwork(networkId);
    if (!network) return;
    
    // 1. Collect sources (machines producing fluid items)
    // 2. Collect sinks (machines consuming fluid items)
    // 3. For each source, route fluid items through pipe network
    // 4. Move 1 fluid item per pipe per tick (same as item pipe speed)
    // 5. If multiple sinks, distribute round-robin
    
    // Reuse item routing logic from Task 4:
    for each source in network.sources:
        for each sink in network.sinks:
            if (sink.acceptsFluid(source.fluidItemId)):
                route = findRoute(source.nodeId, sink.nodeId, {source.fluidItemId, 1});
                m_activeFluidRoutes.push_back(route);
}
```

### 9.5 Fluid type separation
Different fluid types (water, steam, acid) should NOT mix in same pipe:
- When `fluidItemId != 0`, pipe rejects other fluid types
- When `fluidItemId == 0`, pipe accepts first fluid type it receives
- Mixing detection: if two connected pipes have different `fluidItemId` → mark `fluidMixed = true`

### 9.6 Update PipeNetworkService for fluid networks
**Location**: `src/services/pipe_network/PipeNetworkService.cpp`

```cpp
void PipeNetworkService::onBlockChanged(uint16_t block_id, int32_t x, int32_t y, int32_t z) {
    if (isPipeBlock(block_id)) {
        if (isFluidPipe(block_id)) {
            m_manager->rebuildFluidNetworks();
        } else {
            m_manager->rebuildNetworks();  // energy
        }
    }
}
```

## Acceptance Criteria
- [ ] `rebuildFluidNetworks()` creates separate networks for fluid pipes
- [ ] `distributeFluid()` moves fluid items through pipe graph
- [ ] Fluid type separation: water/steam/acid stay in separate pipes
- [ ] Mixed fluid detection: different types → `fluidMixed = true` flag
- [ ] PipeNetworkService triggers fluid rebuild on fluid pipe placement
- [ ] No regression in energy distribution

## Dependencies
- Task 1 (pipe block IDs — fluid_pipe recognition)
- Task 2 (item graph — reuse item routing)
- Task 8 (fluid item IDs)
- Required by: Task 10 (boiler→pipe)

## Files to Modify
- `src/services/pipe_network/PipeNetwork.h/.cpp` — fluid network rebuild + distribution
- `src/services/pipe_network/PipeNetworkService.cpp` — trigger on block change
