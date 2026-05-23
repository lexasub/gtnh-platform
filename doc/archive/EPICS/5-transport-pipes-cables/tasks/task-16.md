# Task 16: Cable Overheat → Explosion (Packet-Triggered)

## Objective
When a packet voltage exceeds cable.maxVoltage (INSTANT heat spike) or packetsThisTick exceeds ampacity (slow heat accumulation), the cable overheats and explodes.

## Requirements

### 16.1 Overheat triggers (packet-based)
**Location**: `src/services/pipe_network/CableGraph.h`

Two independent overheat mechanisms:

**1. Voltage overmatch (INSTANT)**:
- `packet.voltage > cable.maxVoltage` → heat spike of `50° per volt over`
- Example: MV packet (128V) through LV cable (32V) → (128-32)*50 = 4800° → instant explosion

**2. Amp overload (SLOW)**:
- `packetsThisTick > cable.ampacity` → `1° per extra packet per tick`
- Example: 4 packets/tick through LV cable (ampacity=2) → 2° per tick → explosion in ~50 ticks

```cpp
struct CableNode {
    // ... from Task 15 ...
    
    // Overheat state
    float temperature = 0.0f;
    float maxTemperature = 100.0f;  // explodes at this temp
    bool isOverheating = false;
};
```

### 16.2 Per-tick overheat check
**Location**: `src/services/pipe_network/CableGraph.cpp`

```cpp
void CableGraph::tick() {
    for (auto& [id, node] : m_nodes) {
        if (node.packetsThisTick == 0) {
            // Cool down: -2° per tick when idle
            node.temperature = std::max(0.0f, node.temperature - 2.0f);
            node.isOverheating = node.temperature > 0;
            continue;
        }
        
        // Amp overload check
        if (node.packetsThisTick > static_cast<int32_t>(node.ampacity)) {
            float overload = node.packetsThisTick - node.ampacity;
            node.temperature += overload;  // +1° per extra packet/tick
        }
        
        // Voltage check happens DURING packet injection (not here)
        // See 16.3
        
        node.packetsThisTick = 0;  // reset for next tick
        
        // Explosion check
        if (node.temperature >= node.maxTemperature) {
            explodeCable(id);
        }
    }
}
```

### 16.3 Voltage check during packet injection
**Location**: `src/services/pipe_network/CableGraph.cpp`

```cpp
void CableGraph::injectPacket(const EnergyPacket& packet, uint64_t startNodeId) {
    auto it = m_nodes.find(startNodeId);
    if (it == m_nodes.end()) return;
    
    auto& node = it->second;
    
    // VOLTAGE CHECK: packet.voltage > cable.maxVoltage
    if (packet.voltage > node.maxVoltage) {
        // INSTANT heat spike — proportional to overvoltage
        float overVoltage = packet.voltage - node.maxVoltage;
        node.temperature += overVoltage * 50.0f;  // 50° per volt over
        
        // Publish overvoltage event
        publishOvervoltageEvent(node.id, packet.voltage, node.maxVoltage);
        
        // Almost certainly explodes this tick
        if (node.temperature >= node.maxTemperature) {
            explodeCable(node.id);
            return;  // packet destroyed
        }
        
        // Packet is destroyed (voltage mismatch)
        spdlog::warn("Packet {}V destroyed by cable {} (max {}V)", 
                     packet.voltage, node.id, node.maxVoltage);
        return;
    }
    
    // Voltage OK → forward packet
    node.packetsIn.push_back(packet);
}
```

### 16.4 Cable explosion
**Location**: `src/services/pipe_network/CableGraph.cpp`

```cpp
void CableGraph::explodeCable(uint64_t nodeId) {
    auto& node = m_nodes[nodeId];
    
    // 1. Set block to AIR in chunk store
    // 2. Damage adjacent blocks (radius 1):
    //    - 50% chance: adjacent block destroyed
    //    - 25% chance: adjacent block damaged
    // 3. Publish cable.explosion event
    // 4. Remove node from graph
    
    CableExplosionEvent event{
        .pos_x = node.x, .pos_y = node.y, .pos_z = node.z,
        .radius = 1,
        .overvoltage = (node.temperature >= node.maxTemperature)
    };
    
    publishEvent("cable.explosion", event);
    m_nodes.erase(nodeId);
    rebuildGraph();
}
```

### 16.5 Visual effects (client)
**Location**: `src/services/game_client/`

When client receives `cable.explosion`:
- Screen shake: 4px, 300ms
- Sparks: 50 particles, random directions, 500ms life
- Sound: electrical crackle → boom

While `isOverheating == true`:
- Sparks at cable position (2-3 per tick)
- Electrical buzzing sound (audible 10 blocks)
- Cable pulses red (texture tint animation)

## Acceptance Criteria
- [ ] Voltage overmatch: packet.voltage > cable.maxVoltage → 50°/V heat spike
- [ ] Amp overload: packetsThisTick > ampacity → +1°/extra packet/tick
- [ ] Overvoltage destroys packet (no forward)
- [ ] Cable explodes when temperature ≥ maxTemperature
- [ ] Explosion destroys block + 50% chance destroy neighbor
- [ ] Cool down: -2°/tick when idle, -0°/tick when active
- [ ] `cable.explosion` event published to client
- [ ] Sparks + buzzing when overheating
- [ ] Screen shake + particles on explosion

## Dependencies
- Task 14 (cable block IDs)
- Task 15 (CableGraph — packet injection)
- Required by: (none — terminal in cable safety chain)

## Files to Modify
- `src/services/pipe_network/CableGraph.h/.cpp` — overheat + explosion
- `src/services/pipe_network/PipeNetworkService.cpp` — publish events
- `src/services/game_client/` — explosion visuals + spark particles
