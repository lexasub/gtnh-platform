# TASK: ItemStack Data Type

**Layer**: 0

**Status**: Draft

**Epic**: 0-foundation-item-inventory

## Affected Services

| Service | Role |
|---------|------|
| **All services** | Shared protocol type — defined in `core.fbs`, used by every service |

---

## Overview

ItemStack is the universal primitive for items in all services. It represents a single stack of a particular item with quantity and state. Every service — ChunkStore, SimulationCore, PipeNetwork, MetaDB — uses ItemStack to communicate blocks as items, machine outputs, and inventory contents.

This document defines the FlatBuffer schema, field semantics, and zero-item convention. Inventory containers and protocol messages are separate tasks.

---

## FlatBuffer Schema

The schema lives in `src/protocol/flatbuffers/`.

```flatbuffers
namespace Protocol {

table ItemStack {
    item_id: uint16;
    count: uint8;
    meta: uint16;
}

}  // namespace Protocol
```

---

## Field Semantics

### `item_id: uint16`

Identifies the item or block. The value comes from a global registry — either the block registry or the item registry. See the Item ID Registry section below for the exact ranges.

A value of 0 means the stack is empty. This is the **zero-item convention**.

### `count: uint8`

Stack size. Valid values are 1 through 64. The maximum is enforced per-item by the SimulationCore. A value of 0 is invalid and treated as an error.

### `meta: uint16`

State data for the item. On MVP there is no NBT — meta holds:

- Damage value for tools/weapons
- State/damage for blocks treated as items (e.g., lit torch, charged bow)
- Custom properties defined by GTNH

Default (0) means new or unmodified.

---

## Item ID Registry

Item IDs are assigned by the SimulationCore at startup. The registry is read-only after startup.

### Block IDs

Blocks are integers 1 through 65,535. ID 0 is reserved for air (treated as no block).

| Range | Notes |
|-------|-------|
| 1–256 | Vanilla blocks |
| 257–512 | GTNH-specific blocks |
| 513–65535 | Reserved for future expansion |

### Item IDs

Items are integers 1 through 65,535. ID 0 means no item.

| Range | Notes |
|-------|-------|
| 1–256 | Vanilla items |
| 257–512 | GTNH-specific items |
| 513–65535 | Reserved for future expansion |

---

## Zero-Item Convention

**item_id = 0 means an empty stack.**

An empty stack is valid and must not be treated as an error. It appears in:

- Empty inventory slots
- Drop messages when nothing falls
- Output slots with no content

When comparing ItemStacks, treat `item_id = 0` as empty regardless of count or meta values.

---

## File Locations

| File | Location |
|------|----------|
| Schema | `src/protocol/flatbuffers/schema.fbs` |
| C++ types | `src/protocol/flatbuffers/ItemStack.cpp` |
| Go types | `src/protocol/flatbuffers/ItemStack.go` |
| Registry data | `src/simulation_core/item_registry.json` |

---

## Acceptance Criteria

#### Scenario: Empty stack
The system treats `item_id = 0` as an empty slot. Inventory UI shows the slot as blank. Protocol handlers skip any operation that would modify an empty stack.

#### Scenario: Single block as item
ChunkStore returns a block when queried by coordinate. SimulationCore wraps the result in an ItemStack with `item_id` from the block registry, `count = 1`, and `meta = 0`. The item flows through the message router to the client.

#### Scenario: Item with damage
A player places a pickaxe in an inventory. The system sets `meta` to the current damage value (0 = new). When the pickaxe breaks, SimulationCore increments meta by 1 and persists the change in MetaDB.

#### Scenario: Stack size limit
A player attempts to place 65 dirt blocks into one slot. The system caps the count at 64. The excess 1 block either breaks or falls to the ground, depending on context.

#### Scenario: Uninitialized meta
A new dirt block has `meta = 0`. When the block is damaged (e.g., by a tool), the system increments meta. The value persists across saves in MetaDB.

#### Scenario: Block with state
A lit torch has `meta = 1`, an unlit torch has `meta = 0`. The SimulationCore reads meta when checking whether a torch emits light.
