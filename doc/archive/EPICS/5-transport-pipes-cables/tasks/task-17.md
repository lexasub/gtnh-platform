# Task 17: Cable Loss (Per Packet × loss_per_block)

## Objective
Implement EU loss per cable block traversed by a packet. Each hop through a cable reduces the packet's EU value by `loss_per_block`, simulating resistive loss.

## Requirements

### 17.1 Packet energy loss per hop
**Location**: `src/services/pipe_network/CableGraph.cpp`

When a packet moves from one cable node to the next, apply loss:

```cpp
void CableGraph::forwardPacket(uint64_t fromNodeId, uint64_t toNodeId, const EnergyPacket& packet) {
    auto fromIt = m_nodes.find(fromNodeId);
    auto toIt = m_nodes.find(toNodeId);
    if (fromIt == m_nodes.end() || toIt == m_nodes.end()) return;
    
    auto& fromNode = fromIt->second;
    auto& toNode = toIt->second;
    
    // Calculate loss for this hop
    auto* def = getCableDef(getBlockId(fromNode.x, fromNode.y, fromNode.z));
    float hopLoss = def ? def->loss_per_block : 0.0f;
    
    // Reduce packet voltage by loss (loss proportional to voltage)
    // A 32V packet loses 0.05 EU through tin cable = 31.95V delivered
    float deliveredVoltage = packet.voltage - hopLoss;
    
    if (deliveredVoltage <= 0) {
        // Packet fully dissipated
        spdlog::debug("Packet fully dissipated at cable node {}", fromNodeId);
        return;
    }
    
    // Forward with reduced voltage (or track total loss separately)
    EnergyPacket forwarded = packet;
    // Option A: voltage decreases (more realistic — voltage drop)
    // forwarded.voltage = static_cast<uint32_t>(deliveredVoltage);
    // Option B: voltage constant, loss tracked as metadata (simpler)
    // For MVP: Option B — voltage is nominal, loss is metadata
    
    toNode.packetsIn.push_back(forwarded);
    
    // Track total path loss for monitoring
    spdlog::trace("Packet {}V → hop loss {}V at node {}", 
                  packet.voltage, hopLoss, fromNodeId);
}
```

### 17.2 Loss-per-block from CableDef
**Location**: `src/services/pipe_network/CableTypes.h` (from Task 14)

| Cable | loss_per_block | 10 blocks | 50 blocks |
|-------|---------------|-----------|-----------|
| Tin | 0.05 EU | 0.5 EU | 2.5 EU |
| Copper | 0.03 EU | 0.3 EU | 1.5 EU |
| Gold | 0.02 EU | 0.2 EU | 1.0 EU |
| Aluminum | 0.02 EU | 0.2 EU | 1.0 EU |
| Tungsten | 0.01 EU | 0.1 EU | 0.5 EU |
| Platinum | 0.005 EU | 0.05 EU | 0.25 EU |

### 17.4 Transformer as loss mitigation
Transformers (Task 18) can step up voltage for long-distance transmission, reducing current (and thus loss). A step-up transformer reduces loss by tier ratio.

### 17.5 Energy flow tracking
**Location**: `src/protocol/pipe_network.fbs`

```flatbuffers
table EnergyFlowEvent {
    // ... existing fields ...
    cable_loss: int32 = 0;        // NEW: EU lost to cable resistance
    path_length: uint16 = 0;      // NEW: number of cable blocks traversed
}
```

## Acceptance Criteria
- [ ] Cable loss = distance * loss_per_block for each cable segment
- [ ] Loss proportional to path length through cables
- [ ] Shorter paths = less loss (encourages efficient layouts)
- [ ] `EnergyFlowEvent` includes cable_loss field
- [ ] 50% loss threshold logs warning
- [ ] No loss through non-cable nodes (machines, pipes)

## Dependencies
- Task 14 (CableDef with loss_per_block)
- Task 15 (voltage tier checking — for flow tracking)
- Required by: (none — terminal in cable efficiency chain)

## Files to Modify
- `src/services/pipe_network/PipeNetwork.cpp` — calculateCableLoss()
- `src/protocol/pipe_network.fbs` — EnergyFlowEvent fields
