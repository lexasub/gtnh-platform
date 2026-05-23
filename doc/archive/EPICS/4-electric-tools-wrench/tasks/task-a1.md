# Task A1: Tool + Battery Buffer Item Registration

## Objective
Register electric tools (drills, chainsaw, wrench) and battery buffer blocks in items.csv and create their item/block definitions.

## Requirements

### 1.1 Add tool items to items.csv
**Location**: `data/registry/items.csv`

Format (from agent: `id,name,stack_size,meta`):
```
90,gtnh:drill_ulv,1,0
91,gtnh:drill_lv,1,0
92,gtnh:drill_mv,1,0
93,gtnh:drill_hv,1,0
94,gtnh:chainsaw_lv,1,0
95,gtnh:wrench,1,0
```

Tools are `stack_size=1` (non-stackable), no meta.

### 1.2 Add battery buffer blocks to items.csv
```
96,gtnh:battery_buffer_lv,64,0
97,gtnh:battery_buffer_mv,64,0
98,gtnh:battery_buffer_hv,64,0
99,gtnh:charger,64,0
```

### 1.3 Create tool constants header
**Location**: `src/data/registry/ToolIds.h` (NEW)

```cpp
#pragma once
#include <cstdint>

// Tools (stack_size=1)
constexpr uint16_t ITEM_DRILL_ULV    = 90;
constexpr uint16_t ITEM_DRILL_LV     = 91;
constexpr uint16_t ITEM_DRILL_MV     = 92;
constexpr uint16_t ITEM_DRILL_HV     = 93;
constexpr uint16_t ITEM_CHAINSAW_LV  = 94;
constexpr uint16_t ITEM_WRENCH       = 95;

// Battery buffer blocks (stack_size=64)
constexpr uint16_t BLOCK_BATTERY_BUFFER_LV = 96;
constexpr uint16_t BLOCK_BATTERY_BUFFER_MV = 97;
constexpr uint16_t BLOCK_BATTERY_BUFFER_HV = 98;
constexpr uint16_t BLOCK_CHARGER           = 99;

// Tool tier mapping
constexpr uint8_t toolTier(uint16_t itemId) {
    switch (itemId) {
        case ITEM_DRILL_ULV: return 0;
        case ITEM_DRILL_LV:
        case ITEM_CHAINSAW_LV: return 1;
        case ITEM_DRILL_MV: return 2;
        case ITEM_DRILL_HV: return 3;
        default: return 0;
    }
}

// Mining level per tier
constexpr uint8_t miningLevel(uint8_t tier) {
    constexpr uint8_t levels[] = {1, 2, 3, 4};  // ULV→HV
    return (tier < 4) ? levels[tier] : 4;
}
```

### 1.4 Register battery buffers in MachineRegistry?
Battery buffers are machines — they store energy and charge items. Add to a new `infrastructure.csv` or `storage.csv`:
```
battery_buffer_lv, LV, storage, 96
battery_buffer_mv, MV, storage, 97
battery_buffer_hv, HV, storage, 98
charger, LV, storage, 99
```

## Acceptance Criteria
- [ ] 6 tool entries + 4 battery entries in items.csv
- [ ] ToolIds.h with all constants
- [ ] `toolTier(itemId)` returns correct tier per tool
- [ ] `miningLevel(tier)` returns correct mining level
- [ ] Battery buffers registered as machine/storage blocks
- [ ] Tools are non-stackable (stack_size=1)

## Dependencies
- None — registration only
- Required by: Task A2 (protocol), Task A5 (energy storage)

## Files to Modify
- `data/registry/items.csv` — tool + battery entries
- `src/data/registry/ToolIds.h` — NEW: constants
