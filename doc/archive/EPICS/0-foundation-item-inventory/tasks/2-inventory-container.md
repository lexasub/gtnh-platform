# TASK: Inventory Container
**Layer**: 0
**Status**: Draft
**Epic**: 0-foundation-item-inventory

## Affected Services

| Service | Role |
|---------|------|
| **All services** | Shared data type — defines slot layout for player/machine/workbench inventories |

# Overview

The Inventory Container is a fixed-slot data structure holding items. Each slot contains an `ItemStack` or empty state (represented by `item_id=0`). This is a plain value type with no ownership semantics.

This layer defines the invariant inventory types and slot operations. Protocol messages for serializing inventories are defined in a separate task.

# Inventory Types

## Player Inventory

| Type | Slots | Source |
|------|-------|--------|
| Player | 36 + 9 hotbar | MetaDB |

Main inventory holds 36 slots. Hotbar holds 9 slots and shares the same stack as the main inventory at indices 36–44.

## Machine Inventory == Workbench Inventory == Chest Inventory

| Type | Slots | Source |
|------|-------|--------|
| Machine | variable input/output | EntityStateStore |

Input and output slots are machine-specific. The Inventory Container defines the fixed-slot model; machine-specific layouts are configured separately.

Workbench Inventory 

| Type | Slots | Source |
|------|-------|--------|
| Workbench | 3×3 + 1 result | EntityStateStore |

The 3×3 crafting grid and a single output slot.

Chest Inventory

| Type | Slots | Source |
|------|-------|--------|
| Chest | 9×N | EntityStateStore |

Deferred.

# Slot Operations

## Add

Insert an item into a slot. If the slot already contains the same item type, increase the stack size up to the maximum.

## Remove

Remove items from a slot. If removing all items, leave the slot empty (item_id=0).

## Transfer

Move items between slots. When transferring between inventories, the source and destination inventories are treated as a single pool.

## Swap

Exchange items between two slots.

# Empty Slot Convention

An empty slot is represented by `item_id=0`. All metadata fields are undefined for empty slots. Operations on empty slots behave as no-ops unless specified otherwise.

# Stack Size Limits

Standard items stack to 64. Custom stack sizes are defined per item type.

# File Locations

```
src/
├── src/
│   └── components/
│       └── inventory/
│           ├── InventoryContainer.cpp
│           ├── InventoryContainer.h
│           └── ItemStack.h
└── doc/
    └── EPICS/
        └── 0-foundation-item-inventory/
            ├── 0-foundation-item-inventory.md
            └── tasks/
                ├── 0-epic-overview.md
                ├── 1-inventory-item-types.md
                └── 2-inventory-container.md
```

# Acceptance Criteria

## Scenario: Add items to empty slot

**Given** slot 0 is empty (item_id=0)
**When** I add 5 copper ingots (item_id=64)
**Then** slot 0 holds 5 copper ingots

## Scenario: Add items to non-empty slot with same type

**Given** slot 1 holds 20 copper ingots
**When** I add 10 more copper ingots
**Then** slot 1 holds 30 copper ingots

## Scenario: Remove all items from a slot

**Given** slot 2 holds 64 gold ingots
**When** I remove 64 gold ingots
**Then** slot 2 is empty (item_id=0)

## Scenario: Transfer items between inventories

**Given** inventory A holds 10 stone in slot 0
**When** I transfer 5 stone from inventory A to inventory B
**Then** inventory A holds 5 stone in slot 0
And inventory B holds 5 stone

## Scenario: Swap items between slots

**Given** slot 3 holds wood
**And** slot 4 holds iron
**When** I swap slots 3 and 4
**Then** slot 3 holds iron
And slot 4 holds wood

## Scenario: Transfer to full stack

**Given** inventory A holds 50 iron in slot 0
**When** I transfer 50 iron from inventory A to inventory B
**Then** inventory A holds 0 iron in slot 0
And inventory B holds 100 iron (merged with existing stack)

<!-- OMO_INTERNAL_END -->
