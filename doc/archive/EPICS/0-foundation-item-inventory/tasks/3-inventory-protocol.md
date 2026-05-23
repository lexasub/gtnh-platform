# TASK: Inventory Protocol Messages
**Layer**: 0
**Status**: Draft
**Epic**: 0-foundation-item-inventory

## Affected Services

| Service | Role | Direction |
|---------|------|-----------|
| **Protocol** | FlatBuffer schema — `InventoryUpdate`, `InventorySlot` | — |
| **Gateway** | Relay — forwards InventoryUpdate between services and client | Gateway ↔ Client |
| **SimulationCore** | Source — sends inventory changes (machine completion, crafting) | Sim → Gateway → Client |
| **MetaDB** | Source — sends player inventory on login | MetaDB → Gateway → Client |
| **GameClient** | Consumer — renders inventory GUI from InventoryUpdate | Read |

## Overview

The Inventory Protocol defines binary messages for transferring block inventory state between services. InventoryUpdate is a server→client push message sent by SimulationCore through Gateway to the GameClient. InventorySlot represents a single inventory slot containing a block item.

Full inventory snapshots are sent on each update during MVP. Delta-based updates may be introduced later.

## FlatBuffer Schema

```flatbuffers
namespace Protocol {

table InventorySlot {
    item_id: uint16;   // block registry ID
    count: uint8;      // stack size
    meta: uint16;      // block state properties
}

table InventoryUpdate {
    player_id: uint64;
    slots: [InventorySlot];  // full inventory snapshot
}

}  // namespace Protocol
```

### Tables

#### InventorySlot

Represents a single inventory slot holding a stack of block items.

| Field    | Type    | Description                              |
|----------|---------|------------------------------------------|
| item_id  | uint16  | Registry ID of the block item type       |
| count    | uint8   | Number of items in the stack (1-64)      |
| meta     | uint16  | Block state properties (damage, color, etc.) |

#### InventoryUpdate

Server→client push message containing a full inventory snapshot.

| Field      | Type    | Description                              |
|------------|---------|------------------------------------------|
| player_id  | uint64  | Unique identifier of the player          |
| slots      | []      | Array of InventorySlot entries            |

## Message Flow

```
┌─────────────────┐
│ SimulationCore  │
│ EntityStateStore │
└────────┬────────┘
         │ onBlockChanged()
         │   - updates local inventory
         │   - sends InventoryUpdate
         └────┬─────────────────────┐
              │                     │
              │ InventoryUpdate     │
              │                     │
         ┌────▼────┐                │
         │  Gateway│                │
         │         │                │
         │  Push   │                │
         └────┬────┘                │
              │                     │
              │ InventoryUpdate     │
              │                     │
         ┌────▼────┐                │
         │ GameClient│              │
         │           │              │
         │ Store    │              │
         │ Render   │              │
         └──────────┘
```

### Server→Client Flow

1. **SimulationCore** receives a `BlockChanged` event from ChunkStore
2. **EntityStateStore** updates its local inventory snapshot for affected player(s)
3. **SimulationCore** constructs an `InventoryUpdate` message
4. Message travels through **Gateway** via internal push channel
5. **Gateway** forwards to connected **GameClient**
6. **GameClient** stores the snapshot in its local state
7. **GameClient** renders the inventory via ImGui

### Client→Server Flow (Future)

Client→server inventory actions (placing, crafting, transferring) will follow a separate protocol. This document focuses on server→client push updates only.

## Field Descriptions

### InventorySlot

- **item_id**: Registry ID of the block item type. Uses Minecraft-style registry IDs (e.g., `stone=1`, `diamond_ore=63`)
- **count**: Stack size, clamped to 64 (typical Minecraft limit)
- **meta**: Block state properties encoded as a 16-bit integer. Specific bits are reserved for:
  - Bits 0-3: Damage/variant index
  - Bits 4-7: Color/tint data
  - Bits 8-15: Reserved for future use

### InventoryUpdate

- **player_id**: 64-bit unique player identifier, globally unique across the game world
- **slots**: Full inventory snapshot. Inventory size is 27 slots (9×3 grid) plus 9 hotbar slots = 36 total. Each slot is represented as an InventorySlot entry.

## Future Extensibility

- **Delta Encoding**: Replace full snapshot with change deltas (slot added/removed/modified)
- **Compression**: LZ4/Huffman compression for bandwidth reduction
- **Asynchronous Updates**: Batch multiple changes into a single update
- **Crafting Messages**: Define separate messages for crafting input/output
- **Inventory Actions**: Define messages for player→server actions (place, take, craft)
- **Equipment**: Extend InventorySlot with durability and enchantment data

## File Locations

```
src/
├── protocol/
│   └── inventory.fbs        # FlatBuffer schema
├── services/
│   ├── simulation_core/
│   │   └── entity_state_store/
│   │       └── inventory.cpp   # Server-side inventory handling
│   └── gateway/
│       └── push_channel.cpp    # Internal message routing
└── client/
    └── inventory_render.cpp    # ImGui rendering
```

## Acceptance Criteria

#### Scenario: New player joins world

1. Player connects through Gateway
2. Gateway sends ChunkSnapshot (terrain data)
3. SimulationCore sends InventoryUpdate for player
4. Inventory contains default contents (empty or starting items)
5. GameClient renders inventory via ImGui
6. Player sees inventory in debug overlay

#### Scenario: Block placed in world

1. Player breaks block via click
2. ChunkStore broadcasts BlockChanged to SimulationCore
3. EntityStateStore updates inventory: removes broken block, adds drop
4. SimulationCore sends InventoryUpdate
5. Gateway forwards to GameClient
6. GameClient updates local inventory
7. ImGui shows new items in inventory

#### Scenario: Multiple inventory updates

1. Player breaks multiple blocks rapidly
2. Each BlockChanged triggers separate InventoryUpdate
3. Full snapshot is sent each time (redundant but correct)
4. GameClient overwrites old snapshot with new one
5. ImGui reflects latest state

#### Scenario: InventoryUpdate structure validation

1. InventoryUpdate contains required fields: player_id, slots
2. Each InventorySlot contains required fields: item_id, count, meta
3. Slots array length matches expected inventory size (36)
4. No fields are optional or nullable (FlatBuffer defaults to 0)

---

**Generated**: 2026-06-01 | **Branch**: main | **Author**: Sisyphus-Junior