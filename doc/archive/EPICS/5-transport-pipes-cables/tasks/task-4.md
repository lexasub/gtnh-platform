# Task 4: Item Movement — 1 Block/Tick via BFS

## Objective
Implement item movement through the item pipe network at 1 block per tick using BFS pathfinding from source machine to destination machine.

## Requirements

### 4.1 Implement item routing algorithm
**Location**: `src/services/pipe_network/PipeNetwork.cpp`

```cpp
struct ItemRoute {
    std::vector<uint64_t> path;      // node IDs from source to dest
    uint64_t sourceMachineId;         // machine pushing item
    uint64_t destMachineId;           // machine receiving item
    uint16_t itemId;
    uint8_t count;
    size_t currentHop = 0;           // current position along path
};

class PipeNetworkManager {
    // ... existing ...
    
    std::vector<ItemRoute> m_activeRoutes;  // items in transit
    
    // NEW:
    ItemRoute findRoute(uint64_t sourceNode, uint64_t destNode, const ItemSlot& item);
    void advanceItems();              // called each tick — move items 1 hop
    void deliverItem(const ItemRoute& route);  // item arrived at destination
};
```

### 4.2 BFS route finding
**Algorithm**:
1. From source machine's connected item pipe node
2. BFS through item pipe graph, following edges
3. Check each machine node encountered: does it accept this item_id?
4. If destination machine has INPUT role on the connected face → route found
5. Return shortest path (fewest hops)

```cpp
ItemRoute PipeNetworkManager::findRoute(uint64_t sourceNode, uint64_t destNode, const ItemSlot& item) {
    std::queue<uint64_t> queue;
    std::unordered_map<uint64_t, uint64_t> parent;  // node -> predecessor
    std::unordered_set<uint64_t> visited;
    
    queue.push(sourceNode);
    visited.insert(sourceNode);
    
    while (!queue.empty()) {
        uint64_t current = queue.front(); queue.pop();
        
        if (current == destNode) {
            // Reconstruct path
            ItemRoute route;
            route.itemId = item.item_id;
            route.count = item.count;
            for (uint64_t n = destNode; n != sourceNode; n = parent[n])
                route.path.push_back(n);
            route.path.push_back(sourceNode);
            std::reverse(route.path.begin(), route.path.end());
            route.sourceMachineId = sourceNode;
            route.destMachineId = destNode;
            return route;
        }
        
        for (const auto& edge : m_edges) {
            uint64_t neighbor = (edge.nodeA == current) ? edge.nodeB :
                                (edge.nodeB == current) ? edge.nodeA : 0;
            if (neighbor && !visited.count(neighbor) && isItemEdge(edge)) {
                visited.insert(neighbor);
                parent[neighbor] = current;
                queue.push(neighbor);
            }
        }
    }
    return ItemRoute{};  // empty = no route
}
```

### 4.3 Tick-based advancement
**Location**: `src/services/pipe_network/PipeNetwork.cpp`

```cpp
void PipeNetworkManager::advanceItems() {
    std::vector<ItemRoute> delivered;
    
    for (auto& route : m_activeRoutes) {
        route.currentHop++;
        
        if (route.currentHop >= route.path.size()) {
            // Reached destination
            deliverItem(route);
            delivered.push_back(route);
        } else {
            // Move item to next pipe node's buffer
            uint64_t currentNode = route.path[route.currentHop];
            auto& node = m_nodes[currentNode];
            if (node.itemBuffer.size() < node.itemCapacity) {
                node.itemBuffer.push_back({route.itemId, route.count});
            }
            // If buffer full, item stays in previous node (backpressure)
        }
    }
    
    // Remove delivered routes
    for (const auto& route : delivered) {
        m_activeRoutes.erase(
            std::remove(m_activeRoutes.begin(), m_activeRoutes.end(), route),
            m_activeRoutes.end()
        );
    }
}
```

### 4.4 Call advanceItems() on each tick
**Location**: `src/services/pipe_network/PipeNetworkService.cpp`

```cpp
// In PipeNetworkService — add to the tick handler
void PipeNetworkService::onTick() {
    // ... existing energy distribution ...
    m_manager->advanceItems();  // NEW: advance items in transit
}
```

### 4.5 Item delivery to machine
**Location**: `src/services/pipe_network/PipeNetwork.cpp`

```cpp
void PipeNetworkManager::deliverItem(const ItemRoute& route) {
    // 1. Find machine connected to destination pipe node
    // 2. Check machine has INPUT role on connected face
    // 3. Check machine has free slot for item
    // 4. Insert item into machine inventory
    // 5. Publish item.delivered event
    // 6. If machine inventory full → item stays in pipe (backpressure)
}
```

### 4.6 Backpressure
If destination machine is full or no route exists:
- Item stays in the pipe buffer at current position
- Next tick: retry delivery
- After N ticks (configurable, default 100): item is LOST (published as ItemFlowEvent LOST)

## Architecture
```
Tick N:     Item at pipe node 1 (source machine output)
Tick N+1:   Item at pipe node 2 (first pipe)
Tick N+2:   Item at pipe node 3 (second pipe)
...
Tick N+K:   Item delivered to destination machine
```

## Acceptance Criteria
- [ ] `findRoute()` returns valid path through item pipe graph
- [ ] `advanceItems()` moves items 1 hop per call
- [ ] Items are removed from source buffer when advancing
- [ ] Items appear in destination machine buffer on delivery
- [ ] Backpressure: full machine → item stays in pipe
- [ ] Item loss after N ticks produces `ItemFlowEvent(LOST)`
- [ ] No interference with energy/fluid distribution

## Dependencies
- Task 2 (item graph structure)
- Task 3 (PushItemToPipe protocol)
- Required by: Task 5 (pipe→machine insertion)

## Files to Modify
- `src/services/pipe_network/PipeNetwork.h` — ItemRoute struct, new methods
- `src/services/pipe_network/PipeNetwork.cpp` — findRoute, advanceItems, deliverItem
- `src/services/pipe_network/PipeNetworkService.cpp` — call advanceItems on tick
