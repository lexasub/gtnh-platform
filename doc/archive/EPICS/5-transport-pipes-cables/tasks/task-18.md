# Task 18: Transformer Block — Packet Voltage Conversion

## Objective
Implement transformer blocks that convert packets between voltage tiers: step up (MV→HV, reduces amp count) for efficient transmission and step down (HV→MV, increases amp count) for machine consumption.

## Architecture

Transformer работает с пакетами, не с continuous flow:
- **Step Up**: 4 LV packets (32V each) → 1 MV packet (128V). Amp: 4→1.
- **Step Down**: 1 MV packet (128V) → 4 LV packets (32V each). Amp: 1→4.

Transformer сидит между двумя Cable Graph'ами разных voltage tiers. Одна грань = high voltage side, 5 граней = low voltage side.

## Requirements

### 18.1 Transformer block registration
**Location**: `data/registry/items.csv` + `producers.csv` or new config

```
transformer_mv_hv, 86, machine
transformer_hv_ev, 87, machine
```

### 18.2 TransformerComponent
**Location**: `src/services/pipe_network/PipeNetwork.h` or `simulation_core/ECS/components/`

```cpp
struct TransformerComponent {
    uint8_t inputTier;     // e.g., 2 = MV
    uint8_t outputTier;    // e.g., 3 = HV
    bool stepUp;           // true = input→output (step up)
                           // false = output→input (step down)
    // Face config:
    // Single face = high voltage side
    // Other 5 faces = low voltage side
    uint8_t highSideFace;  // 0-5: which face is the high voltage side
};
```

### 18.3 TransformerComponent
**Location**: `src/services/pipe_network/CableGraph.h`

```cpp
struct TransformerDef {
    uint16_t blockId;
    uint8_t inputTier;       // LV=1, MV=2, HV=3
    uint8_t outputTier;      // MV=2, HV=3, EV=4
    bool stepUp;             // true = input→output (step up)
    uint8_t highSideFace;    // 0-5: single face = high voltage side
};

// Conversion ratio between tiers
// stepUp:   amps_in / 4 = amps_out, voltage * 4
// stepDown: amps_in * 4 = amps_out, voltage / 4
constexpr uint32_t TIER_VOLTAGE[] = {8, 32, 128, 512, 2048, 8192};
// Tier 0=ULV(8V), 1=LV(32V), 2=MV(128V), 3=HV(512V), 4=EV(2048V)

constexpr uint32_t tierVoltage(uint8_t tier) {
    return (tier < 5) ? TIER_VOLTAGE[tier] : 0;
}
```

### 18.4 Transformer packet conversion
**Location**: `src/services/pipe_network/CableGraph.cpp`

```cpp
void CableGraph::processTransformer(uint64_t transformerNodeId) {
    auto* tf = getTransformerDef(transformerNodeId);
    if (!tf) return;
    
    auto& node = m_nodes[transformerNodeId];
    
    // Collect all incoming packets
    std::vector<EnergyPacket> lowSidePackets;
    std::vector<EnergyPacket> highSidePackets;
    
    for (const auto& pkt : node.packetsIn) {
        if (pkt.voltage == tierVoltage(tf->inputTier)) {
            lowSidePackets.push_back(pkt);
        } else {
            highSidePackets.push_back(pkt);
        }
    }
    
    if (tf->stepUp) {
        // Step Up: 4 low-voltage packets → 1 high-voltage packet
        // e.g., 4× LV(32V) → 1× MV(128V)
        int completeSets = lowSidePackets.size() / 4;
        for (int i = 0; i < completeSets; i++) {
            EnergyPacket outPkt;
            outPkt.voltage = tierVoltage(tf->outputTier);
            outPkt.ampCount = 1;
            outPkt.targetId = 0;  // broadcast
            // Push to high-side face
            pushPacketToFace(transformerNodeId, tf->highSideFace, outPkt);
        }
        // Remainder packets stay in buffer (not enough for full conversion)
    } else {
        // Step Down: 1 high-voltage packet → 4 low-voltage packets
        // e.g., 1× MV(128V) → 4× LV(32V)
        for (const auto& pkt : highSidePackets) {
            for (int i = 0; i < 4; i++) {
                EnergyPacket outPkt;
                outPkt.voltage = tierVoltage(tf->outputTier);
                outPkt.ampCount = 1;
                outPkt.targetId = 0;
                // Push to one of the 5 low-side faces (round-robin)
                uint8_t face = (tf->highSideFace + 1 + i) % 6;
                if (face == tf->highSideFace) face = (face + 1) % 6;
                pushPacketToFace(transformerNodeId, face, outPkt);
            }
        }
    }
    
    node.packetsIn.clear();
}
```

### 18.5 Voltage tier conversion ratios (packet-based)

| Conversion | Input | Output | Ratio |
|-----------|-------|--------|-------|
| ULV→LV | 8 EU/t | 32 EU/t | 4:1 |
| LV→MV | 32 EU/t | 128 EU/t | 4:1 |
| MV→HV | 128 EU/t | 512 EU/t | 4:1 |
| HV→EV | 512 EU/t | 2048 EU/t | 4:1 |
| LV→ULV | 32 EU/t | 8 EU/t | 1:4 |
| MV→LV | 128 EU/t | 32 EU/t | 1:4 |

### 18.7 GeneratorSystem / MachineSystem integration change

**Важно**: С переходом на packet-based Cable Graph, меняется интеграция:

**GeneratorSystem** — вместо `publishNodeUpdate()` через PipeEnergyClient:
```cpp
// Было:
m_pipeEnergyClient->publishNodeUpdate(pos, energyBuffer, capacity, true);

// Стало:
CableGraph::injectPacket(EnergyPacket{tierVoltage(tier), 1, entityId, 0}, 
                         adjacentCableNodeId);
```

**MachineSystem** — вместо `sendConsumeRequest()`:
```cpp
// Было:
m_pipeEnergyClient->sendConsumeRequest(entityId, pos, energyNeeded);

// Стало:
auto packets = CableGraph::collectPackets(adjacentCableNodeId);
for (auto& pkt : packets) {
    energy.addEnergy(pkt.voltage);  // consume packet EU
}
```

HEAT/STEAM машины остаются на continuous `distributeEnergy()` — их не трогаем.

### 18.8 Transformer face config
**Architecture decision**: Transformer follows 1-face-high, 5-faces-low pattern:
- Step Up: high face = output, 5 faces = input
- Step Down: high face = input, 5 faces = output

This allows clear visual distinction and prevents installation mistakes.

### 18.9 Transformer explosion protection
Transformers protect downstream machines from overvoltage:
- If input packet voltage > transformer input rating → transformer explodes (not the machine)
- Transformer uses CableNode overheat tracking (same as cables, higher threshold)
- Transformer has higher overheat threshold than cables (300° vs 100°)

## Acceptance Criteria
- [ ] Transformer block registered in items.csv
- [ ] `TransformerComponent` with inputTier, outputTier, stepUp, highSideFace
- [ ] Step-up: consumes 4 EU low → produces 1 EU high
- [ ] Step-down: consumes 1 EU high → produces 4 EU low
- [ ] High-side face config enforced (1 face = high, 5 faces = low)
- [ ] Transformer processes on each tick
- [ ] Overvoltage → transformer explodes (not the machine)
- [ ] Client: transformer visual shows high-side face marker
- [ ] Client: transformer model different from cables

## Dependencies
- Task 14 (cable types)
- Task 15 (voltage tier — transformer extends this)
- Required by: (none — terminal in voltage chain)

## Files to Modify
- `data/registry/items.csv` — transformer entries
- `src/services/pipe_network/PipeNetwork.h` — TransformerComponent
- `src/services/pipe_network/PipeNetwork.cpp` — processTransformer()
- `src/services/pipe_network/PipeNetworkService.cpp` — transformer registration
- `src/services/game_client/` — transformer visuals
