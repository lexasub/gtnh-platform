# Task A6: Battery Buffer 20 Hz Tick

## Objective
Implement the Battery Buffer block's tick logic: it receives EU from PipeNetwork (or creative), stores it, and charges tools placed in its inventory slot(s).

## Requirements

### 6.1 BatteryBufferComponent
**Location**: `src/services/simulation_core/ECS/Components/BatteryBufferComponent.h` (NEW)

```cpp
#pragma once
#include <cstdint>

struct BatteryBufferComponent {
    uint32_t capacity;       // EU storage (LV=40000, MV=150000, HV=600000)
    int32_t stored;          // Current EU
    uint8_t tier;            // 0=ULV...3=HV
    int32_t maxInput;        // Max EU/tick input (LV=32, MV=128, HV=512)
    int32_t chargeRate;      // EU/tick per charging slot (LV=8, MV=32, HV=128)
    uint8_t numSlots;        // Charging slots (1 for LV, 2 for MV, 4 for HV)
};
```

### 6.2 BatteryBufferSystem
**Location**: `src/services/simulation_core/ECS/Systems/BatteryBufferSystem.h/.cpp` (NEW)

```cpp
#pragma once
#include <entt/entt.hpp>
#include "ECS/Components/BatteryBufferComponent.h"
#include "ECS/Components/InventoryContainer.h"
#include "Inventory/ItemEnergyStorage.h"

class BatteryBufferSystem {
public:
    void tick(entt::registry& registry, float dt);
    
private:
    void chargeSlot(BatteryBufferComponent& buffer, InventoryContainer& inv, uint8_t slotIdx);
};
```

```cpp
// BatteryBufferSystem.cpp
#include "BatteryBufferSystem.h"

void BatteryBufferSystem::tick(entt::registry& registry, float dt) {
    auto view = registry.view<BatteryBufferComponent, InventoryContainer, Position>();
    
    for (auto entity : view) {
        auto& buffer = view.get<BatteryBufferComponent>(entity);
        auto& inv = view.get<InventoryContainer>(entity);
        
        // Charge each occupied slot
        for (uint8_t i = 0; i < buffer.numSlots && i < inv.slots.size(); i++) {
            if (!inv.slots[i].isEmpty()) {
                chargeSlot(buffer, inv, i);
            }
        }
        
        // Receive energy from PipeNetwork (via energy cable or creative)
        // (integration with cable/power system)
    }
}

void BatteryBufferSystem::chargeSlot(BatteryBufferComponent& buffer, 
                                       InventoryContainer& inv, uint8_t slotIdx) {
    auto& slot = inv.slots[slotIdx];
    uint16_t itemId = slot.item_id;
    
    // Check if item is chargeable
    auto it = TOOL_ENERGY_DEFS.find(itemId);
    if (it == TOOL_ENERGY_DEFS.end()) return;
    
    const auto& def = it->second;
    int32_t currentEnergy = getToolEnergy(slot);
    
    // Check if already full
    if (currentEnergy >= def.capacity) return;
    
    // Calculate charge this tick
    int32_t energyToTransfer = std::min({
        buffer.chargeRate,                    // buffer charge rate
        buffer.stored,                         // available EU in buffer
        def.capacity - currentEnergy,          // space in tool
        def.maxInput                           // tool's max input rate
    });
    
    if (energyToTransfer <= 0) return;
    
    // Transfer energy
    buffer.stored -= energyToTransfer;
    setToolEnergy(slot, currentEnergy + energyToTransfer);
}
```

### 6.3 Battery buffer capacity per tier

| Tier | Capacity | maxInput | chargeRate | Slots | Block ID |
|------|----------|----------|------------|-------|----------|
| LV   | 40,000   | 32 EU/t  | 8 EU/t     | 1     | 96       |
| MV   | 150,000  | 128 EU/t | 32 EU/t    | 2     | 97       |
| HV   | 600,000  | 512 EU/t | 128 EU/t   | 4     | 98       |

### 6.4 Charger block
Charger (block_id=99) is similar but:
- Can charge any tier tool (not limited by buffer tier)
- Receives EU from higher-tier cable
- Transformer built-in: steps down voltage / limits charge rate

### 6.5 Energy input integration
Battery buffer connects to PipeNetwork energy/cable graph:
- Has ENERGY role face (side_config from Task B1)
- Receives EU packets/tick from connected cable
- Fills `buffer.stored` up to `buffer.capacity`

## Acceptance Criteria
- [ ] BatteryBufferComponent defined
- [ ] BatteryBufferSystem charges tools in slots each tick
- [ ] Charge respects: tool capacity, buffer stored, chargeRate, maxInput
- [ ] Different tier buffers have different stats (LV/MV/HV)
- [ ] Full tool → no charge (no energy wasted)
- [ ] Empty buffer → no charge until EU arrives
- [ ] Battery buffer receives energy from connected cable
- [ ] Charger block supports any tier tool

## Dependencies
- Task A1 (battery buffer block IDs)
- Task A4 (ItemEnergyStorage — tool charge API)
- Epic 5 Task 14-15 (cable energy delivery)
- Task B1 (side_config — ENERGY face)

## Files to Create/Modify
- `src/services/simulation_core/ECS/Components/BatteryBufferComponent.h` — NEW
- `src/services/simulation_core/ECS/Systems/BatteryBufferSystem.h/.cpp` — NEW
- `src/services/simulation_core/main.cpp` — register BatteryBufferSystem tick
