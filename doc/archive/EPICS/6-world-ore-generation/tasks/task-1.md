# Task 1: Ore Item ID Registration

## Objective
Ensure all ore item IDs are properly registered in `data/registry/items.csv` and accessible from code. Currently iron_ore=3, gold_ore=5, tin_ore=25, copper_ore=53 exist — need coal, redstone, lapis, diamond.

## Requirements

### 1.1 Add missing ore entries to items.csv
**Location**: `data/registry/items.csv`

**Current entries** (from explore agent):
```
iron_ore, 3
gold_ore, 5
tin_ore, 25
copper_ore, 53
```

**Add missing ores**:
```
coal_ore, 70
redstone_ore, 71
lapis_ore, 72
diamond_ore, 73
```

Verify exact CSV format (header row, delimiter) from existing entries.

### 1.2 Add ore constants to code
**Location**: `src/services/world_generator/OreTypes.h` (NEW)

```cpp
#pragma once
#include <cstdint>

constexpr uint16_t ORE_IRON    = 3;
constexpr uint16_t ORE_GOLD    = 5;
constexpr uint16_t ORE_TIN     = 25;
constexpr uint16_t ORE_COPPER  = 53;
constexpr uint16_t ORE_COAL    = 70;
constexpr uint16_t ORE_REDSTONE = 71;
constexpr uint16_t ORE_LAPIS   = 72;
constexpr uint16_t ORE_DIAMOND = 73;
```

### 1.3 Verify block-to-item mapping
Each ore block_id must match the item_id used in drops. For MVP: ore block = ore item (1:1 drop ratio). Verify that `items.db` (SQLite) has matching entries — if not, add them.

## Acceptance Criteria
- [ ] All 8 ore types in items.csv
- [ ] Ore constants header compiles
- [ ] Ore block_id = ore item_id (1:1 drop)
- [ ] No ID conflicts with existing blocks
- [ ] items.db matches items.csv

## Dependencies
- None — registry changes only
- Required by: Task 2 (ore config), Task 3 (generation)

## Files to Modify
- `data/registry/items.csv` — add missing ores
- `src/services/world_generator/OreTypes.h` — NEW: constants
