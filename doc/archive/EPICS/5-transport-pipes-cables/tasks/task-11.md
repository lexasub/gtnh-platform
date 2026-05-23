# Task 11: Infinite Water Source + Fluid Gravity

## Objective
Implement an infinite water source placeholder (for testing fluid pipes) and gravity simulation for fluids (fluids flow downward when no connection below).

## Requirements

### 11.1 Infinite water source block
**Location**: `src/services/chunk_store/` + `data/registry/items.csv`

New block: `infinite_water_source` — behaves like CreativeGenerator but for water:

```cpp
// Block definition
constexpr uint16_t BLOCK_ID_INFINITE_WATER_SOURCE = 68;

// Behavior:
// - Always has infinite water (fluidBuffer = max, isFluidSource = true)
// - Adjacent fluid pipe can extract water endlessly
// - No energy requirement
// - Visual: blue block with infinite symbol (∞)
```

**Registration**:
- Add to `items.csv`
- Add to a new `infrastructure.csv` or keep in `producers.csv` with `energy_label=NONE`
- PipeNetwork recognises as fluid source node

### 11.2 Fluid gravity implementation
**Location**: `src/services/pipe_network/PipeNetwork.cpp`

Fluids fall downward when:
1. Pipe node has fluid item
2. Face DOWN is connected to another fluid pipe
3. Item should move DOWN even without a machine destination (gravity flow)

```cpp
void PipeNetworkManager::applyFluidGravity() {
    for (auto& [id, node] : m_nodes) {
        if (!node.isFluidNode() || node.fluidBuffer.empty()) continue;
        
        // Check if there is a pipe below
        uint64_t belowNodeId = getNodeAt(node.x, node.y - 1, node.z);
        if (belowNodeId && isFluidEdge(getEdge(id, belowNodeId))) {
            auto& below = m_nodes[belowNodeId];
            
            // Move fluid item down if space available
            if (below.fluidBuffer.size() < below.fluidCapacity) {
                ItemSlot item = node.fluidBuffer.back();
                node.fluidBuffer.pop_back();
                below.fluidBuffer.push_back(item);
                
                // Gravity is faster: 3 blocks/tick
                // If below node also has below connection → keep falling
            }
        }
    }
}
```

**Gravity speed**: fluids flow 3 blocks/tick downward vs 1 block/tick regular flow.

### 11.3 Fluid accumulation at lowest point
If a fluid pipe column ends (no pipe below), item accumulates in the bottom-most pipe node until:
- A machine with FLUID_INPUT role is below
- The pipe continues horizontally
- Buffer is full → backpressure fills the column upward

### 11.4 Integration with fluid network rebuild
**Location**: `src/services/pipe_network/PipeNetworkService.cpp`

```cpp
void PipeNetworkService::onTick() {
    // ... existing tick ...
    
    // NEW: apply fluid gravity before advancing items
    m_manager->applyFluidGravity();
    m_manager->advanceItems();  // from Task 4
    m_manager->distributeFluid();  // from Task 9
}
```

## Acceptance Criteria
- [ ] `infinite_water_source` block placed and recognized by PipeNetwork
- [ ] Fluid pipe connected to water source → pipe receives water items
- [ ] Gravity: fluid moves 3 blocks/tick downward
- [ ] Without downward connection, fluid stays in lowest pipe node
- [ ] Backpressure: full pipe column stops water extraction
- [ ] No energy required for infinite water source

## Dependencies
- Task 8 (fluid item IDs — water)
- Task 9 (fluid graph — pipe transport)
- GameClient block registry (for infinite_water_source visual)

## Files to Modify
- `data/registry/items.csv` — infinite_water_source entry
- `src/services/pipe_network/PipeNetwork.h/.cpp` — applyFluidGravity()
- `src/services/pipe_network/PipeNetworkService.cpp` — call gravity on tick
- `src/services/chunk_store/` — block registration
