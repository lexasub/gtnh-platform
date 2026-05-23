# Task 6: Ore Mining by Hand (No Tools — MVP)

## Objective
Ensure ore blocks can be mined by hand (any item or empty hand) and drop the correct ore item. For MVP, ores drop themselves as blocks (1:1).

## Requirements

### 6.1 Block hardness for ores
**Location**: `data/registry/` — block hardness config (possibly `blocks.csv` or inline)

Ore blocks need higher hardness than stone to justify tools later:
```
ore_iron,     hardness=3.0
ore_gold,     hardness=3.0
ore_copper,   hardness=3.0
ore_tin,      hardness=3.0
ore_coal,     hardness=3.0
ore_redstone, hardness=3.0
ore_lapis,    hardness=3.0
ore_diamond,  hardness=3.0
stone,        hardness=1.5
```

For MVP: any item/tool can break any ore. Hardness only affects break time, not tool requirement.

### 6.2 Block break → drop logic
**Location**: `src/services/chunk_store/World/ServerWorld.cpp` or CAS handler

When a block is broken:
1. If block_id is an ore (check against OreConfig)
2. Drop item with item_id = block_id (1:1)
3. Items go to player inventory (via existing MetaDB flow)

```cpp
// In SetBlockCASHandler or break handler
if (OreConfig::instance().isOre(block_id)) {
    // Drop ore item (1:1)
    inventoryStore->giveItem(player_id, block_id, 1);
    // Block becomes air
}
```

### 6.3 Break time
Break time = hardness * 1000ms / tool_power
- Hand: tool_power = 1.0 (100% of hardness time)
- For MVP: instant break (no progress bar) ← until tool system is in

## Acceptance Criteria
- [ ] Ore blocks break with left-click (any tool/hand)
- [ ] Breaking ore drops the correct item_id (iron_ore drops iron_ore item)
- [ ] Item appears in player inventory
- [ ] Stone drops nothing (by existing logic) — unchanged
- [ ] Non-ore blocks unaffected
- [ ] Block breaking respects chunk boundaries

## Dependencies
- Task 1 (ore item IDs)
- Existing chunk_store block break logic

## Files to Modify
- `data/registry/` — hardness values (if separate config)
- `src/services/chunk_store/World/ServerWorld.cpp` — drop logic for ores
