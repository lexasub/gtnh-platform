# Task 14: Cable Block IDs + isCableBlock()

## Objective
Register cable blocks per voltage tier and extend PipeNetworkService to recognize them via `isCableBlock()`.

## Архитектурный контекст

Cables формируют **отдельный Cable Graph** (packet-based), не входящий в Energy Graph (continuous flow для HEAT).

```
PipeNetworkManager
├── Energy Graph (continuous) — HEAT only
│   └── distributeEnergy(), scalar energyBuffer
├── Cable Graph (packet-based) — ELECTRICITY only
│   └── EnergyPacket{voltage, amp_count}, request/response
├── Fluid Graph (items-as-fluids) — STEAM + fluids
└── Item Graph (BFS routing) — items
```

Cable graph использует packet-based модель GT:
- Generator создаёт `EnergyPacket(voltage=32, amp=1)` каждый тик
- Machine запрашивает пакеты через `energy.consume.request`
- Cable проверяет `packet.voltage ≤ cable.max_voltage`
- Cable считает `packets_this_tick ≤ cable.ampacity`

## Requirements

### 14.1 Register cable block IDs
**Location**: `data/registry/items.csv`

```
cable_tin, 80, infrastructure
cable_copper, 81, infrastructure
cable_gold, 82, infrastructure
cable_alu, 83, infrastructure
cable_tungsten, 84, infrastructure
cable_platinum, 85, infrastructure
transformer_mv_hv, 86, machine
transformer_hv_ev, 87, machine
```

### 14.2 Cable constants
**Location**: `src/services/pipe_network/CableTypes.h` (NEW)

```cpp
#pragma once
#include <cstdint>
#include <unordered_map>

struct CableDef {
    uint16_t block_id;
    uint8_t tier;            // 0=ULV ... 9=UHV
    const char* name;
    float loss_per_block;    // EU loss per meter
    uint32_t max_voltage;    // EU/t before overheat
    uint32_t ampacity;       // max EU/t throughput
};

const std::unordered_map<uint16_t, CableDef> CABLE_DEFS = {
    {80, {80, 1, "cable_tin",      0.05f, 32,    32}},
    {81, {81, 1, "cable_copper",   0.03f, 32,    64}},
    {82, {82, 2, "cable_gold",     0.02f, 128,   128}},
    {83, {83, 2, "cable_alu",      0.02f, 128,   256}},
    {84, {84, 3, "cable_tungsten", 0.01f, 512,   512}},
    {85, {85, 4, "cable_platinum", 0.005f, 2048, 1024}},
};

inline bool isCableBlock(uint16_t block_id) {
    return CABLE_DEFS.count(block_id) > 0;
}

inline const CableDef* getCableDef(uint16_t block_id) {
    auto it = CABLE_DEFS.find(block_id);
    return it != CABLE_DEFS.end() ? &it->second : nullptr;
}
```

### 14.3 isCableBlock() in PipeNetworkService
**Location**: `src/services/pipe_network/PipeNetworkService.h/.cpp`

```cpp
bool PipeNetworkService::isCableBlock(uint16_t block_id) const {
    return ::isCableBlock(block_id);
}
```

### 14.4 Cable blocks in MachineRegistry?
Cables are infrastructure, not machines. They should NOT appear in consumers.csv/producers.csv. Add a skip check in machine entity creation.

## Acceptance Criteria
- [ ] 6 cable types + 2 transformer entries in items.csv
- [ ] `CableDef` with tier, loss, max_voltage, ampacity
- [ ] `isCableBlock()` returns true for block IDs 80-85
- [ ] `getCableDef()` returns correct CableDef per block_id
- [ ] Cable blocks do NOT create ECS machine entities
- [ ] Cable blocks recognized by PipeNetworkService

## Dependencies
- Task 1 (pipe block ID pattern)
- Required by: Task 15 (voltage checking), Task 16 (overheat)

## Files to Modify
- `data/registry/items.csv` — cable entries
- `src/services/pipe_network/CableTypes.h` — NEW: definitions
- `src/services/pipe_network/PipeNetworkService.h/.cpp` — isCableBlock()
