# Task 15: Packet-Based Cable Graph (Voltage + Amp Checking)

## Objective
Implement a separate packet-based Cable Graph for ELECTRICITY type. Generator produces `EnergyPacket{voltage, amp_count}`, machine requests packets, cable checks `packet.voltage ≤ cable.max_voltage` and `packets_this_tick ≤ cable.ampacity`.

> **Статус**: 🟡 **WIP — class skeleton + registration DONE, packet operations pending**
> - ✅ CableGraph class (CableGraph.h/.cpp) — EnergyPacket, CableNode, all methods declared
> - ✅ CableTypes.h — CableDef, CABLE_DEFS, isCableBlock(), getCableDef()
> - ✅ registerGenerator/registerMachine — реализованы и вшарены (2026-06-27)
> - ✅ unregisterGenerator/unregisterMachine — добавлены (2026-06-27)
> - ❌ injectPacket — объявлен, тело пустое
> - ❌ collectPackets — объявлен, тело пустое
> - ❌ tick() — объявлен, тело пустое
> - ❌ GeneratorSystem — не создаёт EnergyPackets, публикует только energy.node.update
> - ❌ MachineSystem — не собирает пакеты

## Requirements

### 15.1 EnergyPacket struct
**Location**: `src/services/pipe_network/PipeNetwork.h` (NEW: CableGraph.h or inline)

```cpp
struct EnergyPacket {
    uint32_t voltage;        // Packet voltage (LV=32, MV=128, HV=512)
    uint8_t ampCount;        // How many amps in this packet (typicially 1)
    uint64_t sourceId;       // Generator ECS entity ID
    uint64_t targetId;       // Target machine ECS entity ID (0=broadcast)
    uint8_t tickIssued;      // Tick when packet was created (for timeout)
};
```

### 15.2 CableGraph class
**Location**: `src/services/pipe_network/CableGraph.h/.cpp` (NEW — separate from PipeNetworkManager)

```cpp
class CableGraph {
public:
    // Node management
    void addCableNode(uint64_t nodeId, const CableDef& def, int32_t x, int32_t y, int32_t z);
    void removeCableNode(uint64_t nodeId);
    void rebuildGraph();  // BFS through cable nodes only
    
    // Packet operations
    void injectPacket(const EnergyPacket& packet, uint64_t startNodeId);
    std::vector<EnergyPacket> collectPackets(uint64_t machineNodeId);
    
    // Per-tick
    void tick();  // advance packets 1 hop, check voltage, track amps
    
    // Cable params
    void setCableParams(uint64_t nodeId, const CableDef& def);
    
private:
    struct CableNode {
        uint64_t id;
        int32_t x, y, z;
        uint8_t tier;
        uint32_t maxVoltage;
        uint32_t ampacity;
        int32_t packetsThisTick;     // reset each tick
        float temperature;
        std::vector<EnergyPacket> packetsIn;   // incoming this tick
        std::vector<EnergyPacket> packetsOut;  // to forward next tick
    };
    
    std::unordered_map<uint64_t, CableNode> m_nodes;
    std::vector<std::vector<uint64_t>> m_cableNetworks;  // BFS groupings
    std::unordered_map<uint64_t, uint64_t> m_generatorToCable;  // gen → first cable
    std::unordered_map<uint64_t, uint64_t> m_machineToCable;    // machine → adjacent cable
};
```

### 15.3 Packet flow
```
GeneratorSystem::tick():
  → creates EnergyPacket{voltage=32, amp=1, sourceId, targetId=0}
  → calls CableGraph::injectPacket(packet, adjacentCableNodeId)

CableGraph::tick():
  → for each cable network:
    → receive packets from generators
    → for each packet:
      → check packet.voltage ≤ each cable.maxVoltage along path
      → if OK: forward packet 1 hop (store in packetsOut)
      → if FAIL: heat spike on weakest cable (→ Task 16)
    → track packetsThisTick per cable node
    → if packetsThisTick > ampacity: heat accumulation (→ Task 16)
    → packets reaching machine node → store in packetsOut for collection

MachineSystem::tick():
  → calls CableGraph::collectPackets(adjacentCableNodeId)
  → consumes packets (deduct voltage per packet into EnergyStorage)
```

### 15.4 Packet routing (BFS through Cable Graph)
When a packet reaches a branch in the cable network:
- If packet has `targetId != 0`: route only toward that machine
- If packet has `targetId == 0` (broadcast): split across all branches
- Dead end with no machine → packet dissipates (energy lost)

```cpp
std::vector<uint64_t> CableGraph::findPath(uint64_t fromCableNode, uint64_t toMachineNode) {
    // BFS through cable nodes only (not through machines)
    // Return cable node IDs from source cable to target machine's adjacent cable
}
```

### 15.5 Voltage tier per cable
Set from `CableDef` (Task 14) on block placement:
```cpp
void CableGraph::setCableParams(uint64_t nodeId, const CableDef& def) {
    auto it = m_nodes.find(nodeId);
    if (it == m_nodes.end()) return;
    it->second.maxVoltage = def.max_voltage;
    it->second.ampacity = def.ampacity;
    it->second.tier = def.tier;
}
```

### 15.6 Generator → Cable → Machine registration
**Location**: `src/services/pipe_network/CableGraph.cpp`

```cpp
// Generator connects to adjacent cable on ENERGY face
void CableGraph::registerGenerator(uint64_t genEntityId, int32_t x, int32_t y, int32_t z) {
    uint64_t cableNode = findAdjacentCable(x, y, z);
    if (cableNode) m_generatorToCable[genEntityId] = cableNode;
}

// Machine connects to adjacent cable on ENERGY face
void CableGraph::registerMachine(uint64_t machineEntityId, int32_t x, int32_t y, int32_t z) {
    uint64_t cableNode = findAdjacentCable(x, y, z);
    if (cableNode) m_machineToCable[machineEntityId] = cableNode;
}
```

### 15.7 Voltage tier in pipe_network.fbs
**Location**: `src/protocol/pipe_network.fbs`

```flatbuffers
table EnergyPacketDef {
    voltage: uint32;
    amp_count: uint8;
    source_id: uint64;
    target_id: uint64;
}

// NEW: for cable graph status
table CableNodeStatus {
    node_id: uint64;
    voltage_tier: uint8;
    max_voltage: uint32;
    ampacity: uint32;
    packets_this_tick: int32;
    temperature: float;
}
```

## Acceptance Criteria
- [x] `CableGraph` class separate from `PipeNetworkManager` — **✅ DONE**
- [x] `EnergyPacket{voltage, amp_count}` struct defined — **✅ DONE**
- [x] CableNode struct with tier, maxVoltage, ampacity, temperature, packetsIn/Out — **✅ DONE**
- [x] registerGenerator/registerMachine/unregisterGenerator/unregisterMachine — **✅ DONE + wired**
- [x] isRegisteredGenerator() — **✅ DONE**
- [ ] Generator creates packets on each tick (currently only energy.node.update)
- [ ] Machine collects packets on each tick
- [ ] Packet voltage checked against cable.max_voltage per hop
- [ ] Packet forward: 1 hop/tick (like item pipes)
- [ ] packetsThisTick tracked per cable node (struct field exists, increment not implemented)
- [ ] packetsThisTick > ampacity → heat accumulation
- [ ] Broadcast packets (targetId=0) split at branches
- [ ] Dead-end packet → energy dissipated

## Dependencies
- Task 14 (cable block IDs + CableDef)
- GeneratorSystem — inject packets instead of publish energy.node.update
- MachineSystem — collect packets instead of sendConsumeRequest
- Required by: Task 16 (overheat), Task 17 (cable loss), Task 18 (transformer)

## Files to Create/Modify
- `src/services/pipe_network/CableGraph.h/.cpp` — NEW: packet-based cable graph
- `src/services/pipe_network/PipeNetworkService.cpp` — cable registration
- `src/services/simulation_core/ECS/Systems/GeneratorSystem.cpp` — inject packets
- `src/services/simulation_core/ECS/Systems/MachineSystem.cpp` — collect packets
- `src/protocol/pipe_network.fbs` — EnergyPacketDef, CableNodeStatus
