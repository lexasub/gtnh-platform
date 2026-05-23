# Task B8: PipeNetwork BFS with side_config

## Objective
Make PipeNetwork BFS routing respect machine side_config roles. A machine's INPUT face accepts items from pipes, OUTPUT face allows extraction, ENERGY face connects to cables, NONE face is blocked.

## Requirements

### 8.1 Side-aware neighbor detection
**Location**: `src/services/pipe_network/PipeNetwork.cpp`

When PipeNetwork discovers neighbors for BFS, check the machine's side_config:

```cpp
struct NeighborInfo {
    uint64_t nodeId;
    uint8_t face;           // Which face of the machine this connects to
    SideRole role;          // The role of that face
};

std::vector<NeighborInfo> PipeNetworkManager::getMachineNeighbors(uint64_t machineNodeId) {
    std::vector<NeighborInfo> neighbors;
    auto& machineNode = m_nodes[machineNodeId];
    
    // Check each of 6 faces
    for (uint8_t face = 0; face < 6; face++) {
        BlockPos neighbor = machineNode.pos + faceOffset[face];
        uint64_t neighborNodeId = getNodeAt(neighbor);
        if (!neighborNodeId) continue;
        
        // Get machine's side_config for this face
        uint8_t role = getMachineSideRole(machineNodeId, face);
        
        // NONE face = no connection (skip)
        if (role == static_cast<uint8_t>(SideRole::NONE)) continue;
        
        // Check if the neighbor is a compatible pipe/cable type
        auto& neighborNode = m_nodes[neighborNodeId];
        if (isCableBlock(neighborNode.blockId) && role == static_cast<uint8_t>(SideRole::ENERGY)) {
            neighbors.push_back({neighborNodeId, face, static_cast<SideRole>(role)});
        } else if (isPipeBlock(neighborNode.blockId) && 
                   (role == static_cast<uint8_t>(SideRole::INPUT) || 
                    role == static_cast<uint8_t>(SideRole::OUTPUT) ||
                    role == static_cast<uint8_t>(SideRole::ANY))) {
            neighbors.push_back({neighborNodeId, face, static_cast<SideRole>(role)});
        } else if (isFluidPipeBlock(neighborNode.blockId) &&
                   (role == static_cast<uint8_t>(SideRole::FLUID_IN) ||
                    role == static_cast<uint8_t>(SideRole::FLUID_OUT) ||
                    role == static_cast<uint8_t>(SideRole::ANY))) {
            neighbors.push_back({neighborNodeId, face, static_cast<SideRole>(role)});
        }
    }
    
    return neighbors;
}
```

### 8.2 Input/Output direction enforcement
When routing items through the network, enforce direction:

```cpp
// Item can only enter a machine through INPUT face
bool canMachineAcceptItem(uint64_t machineNodeId, uint8_t incomingFace, uint16_t itemId) {
    uint8_t role = getMachineSideRole(machineNodeId, incomingFace);
    return role == static_cast<uint8_t>(SideRole::INPUT) || 
           role == static_cast<uint8_t>(SideRole::ANY);
}

// Item can only leave a machine through OUTPUT face
bool canMachineEjectItem(uint64_t machineNodeId, uint8_t outgoingFace, uint16_t itemId) {
    uint8_t role = getMachineSideRole(machineNodeId, outgoingFace);
    return role == static_cast<uint8_t>(SideRole::OUTPUT) || 
           role == static_cast<uint8_t>(SideRole::ANY);
}
```

### 8.3 Energy connection through ENERGY face only
```cpp
// Energy cables connect only through ENERGY face
bool canConnectCable(uint64_t machineNodeId, uint8_t face) {
    uint8_t role = getMachineSideRole(machineNodeId, face);
    return role == static_cast<uint8_t>(SideRole::ENERGY);
}
```

### 8.4 Subscribe to machine.config.updated
**Location**: `src/services/pipe_network/PipeNetworkService.cpp`

```cpp
void PipeNetworkService::subscribeToConfigUpdates() {
    m_router->subscribe("machine.config.updated", [this](const Message& msg) {
        auto* event = flatbuffers::GetRoot<Protocol::MachineConfigUpdated>(msg.data());
        
        // Update side config cache
        uint64_t nodeId = getNodeId(event->pos()->x(), event->pos()->y(), event->pos()->z());
        if (nodeId) {
            SideConfigCache config;
            for (int i = 0; i < 6 && i < event->faces()->size(); i++) {
                config.faces[i] = event->faces()->Get(i);
            }
            m_sideConfigCache[nodeId] = config;
            
            // Rebuild networks — topology may have changed
            rebuildNetworks();
            rebuildItemNetworks();
            rebuildFluidNetworks();
        }
    });
}
```

### 8.5 SideConfig cache in PipeNetwork
**Location**: `src/services/pipe_network/PipeNetworkService.h`

```cpp
struct SideConfigCache {
    uint8_t faces[6] = {5, 5, 5, 5, 5, 5};  // default: all ANY
    uint64_t lastUpdated = 0;
};

// Cache machine side configs (populated from machine.config.updated)
std::unordered_map<uint64_t, SideConfigCache> m_sideConfigCache;

uint8_t getMachineSideRole(uint64_t nodeId, uint8_t face) {
    auto it = m_sideConfigCache.find(nodeId);
    if (it != m_sideConfigCache.end() && face < 6) {
        return it->second.faces[face];
    }
    return 5;  // default: ANY
}
```

### 8.6 Default behavior when no config cached
If PipeNetwork hasn't received side config for a machine yet (e.g., just started, config not yet published):
- All faces = ANY (allow all connections)
- Once machine.config.updated received → enforce roles

## Acceptance Criteria
- [ ] BFS routing checks side_config before connecting
- [ ] NONE face → no connection (blocked)
- [ ] ENERGY face → cables only
- [ ] INPUT face → items enter only
- [ ] OUTPUT face → items exit only
- [ ] FLUID_IN/FLUID_OUT → fluid pipes only
- [ ] ANY face → allow all (default behavior)
- [ ] Subscribe to `machine.config.updated` for live updates
- [ ] Network rebuild triggered on config change
- [ ] Default (no config) = ANY, no broken connections

## Dependencies
- Task B1 (side_config roles enum)
- Task B6 (machine.config.updated event)
- Epic 5 Tasks (PipeNetwork graphs)
- Epic 5 Task 14-15 (cable energy routing)

## Files to Modify
- `src/services/pipe_network/PipeNetwork.h/.cpp` — side-aware BFS
- `src/services/pipe_network/PipeNetworkService.h/.cpp` — config cache + subscriber
