# TASK: Storage Architecture Guidelines
**Layer**: 0
**Status**: Draft
**Epic**: 0-foundation-item-inventory

## Affected Services

| Service | Role | R/W |
|---------|------|-----|
| **MetaDB** (Go/SQLite) | Primary for player inventories — keyed by `player_id` | Read/Write |
| **EntityStateStore** ⬅️ NEW (C++/LMDB) | Primary for world TileEntity state — keyed by `dim\|x\|y\|z` | Read/Write |
| **ChunkStore** (C++/LMDB) | Block + meta-layer only. Never inventories. | Read/Write |
| **SimulationCore** | Post-hook — subscribes to inventory events for gameplay | Event subscriber |
| **Gateway** | Relay — routes inventory/entity data to/from clients | Relay |

---

# Overview

Inventory data in GTNH Platform must be separated from block data. Three storage systems exist, each with a distinct purpose:

- **MetaDB** — player-bound data (player inventory, equipment)
- **EntityStateStore** — world-bound data (machine state, workbench, chests)
- **ChunkStore** — block and meta-layer only. Never inventories.

The separation prevents data corruption, optimizes read/write paths, and respects the semantic boundary between what belongs to a player and what belongs to a location.

---

# Storage Separation Rules

## Rule 1: Player Data in MetaDB

Player-owned inventories always live in MetaDB. This includes:

- Personal inventory slots
- Hotbar
- Equipment (armor, tools, held item)

When a player logs in, MetaDB returns the snapshot. When they log out, MetaDB serializes the snapshot to disk. The data persists independently of the world.

## Rule 2: World Data in EntityStateStore

TileEntity state lives in EntityStateStore. This includes:

- Machine internal inventories (furnace fuel/recipe slots)
- Workbench storage
- Chest contents
- Hopper contents

EntityStateStore uses coordinate keys (`x, y, z`) to locate TileEntity state. The store is optimized for frequent reads and writes because machines update every tick.

## Rule 3: ChunkStore Never Stores Inventories

ChunkStore stores only:

- Block IDs
- Block metadata (damage, facing direction, etc.)
- Meta-layer (multiblock IDs)

ChunkStore never stores inventory data — not item stacks, not counts, not NBT. A block is a block. What it contains is separate.

---

# Where Each Inventory Lives

## Personal Inventory

| Component | Store | Key |
|-----------|-------|-----|
| Player inventory slots | MetaDB | `player_id` |
| Hotbar | MetaDB | `player_id` |
| Equipment | MetaDB | `player_id` |

## World Invenories

| Component | Store | Key |
|-----------|-------|-----|
| Furnace internal | EntityStateStore | `x, y, z` |
| Workbench internal | EntityStateStore | `x, y, z` |
| Chest contents | EntityStateStore | `x, y, z` |
| Hopper contents | EntityStateStore | `x, y, z` |

## Block Data

| Component | Store | Key |
|-----------|-------|-----|
| Block IDs | ChunkStore | `x, y, z` |
| Block metadata | ChunkStore | `x, y, z` |
| Multiblock IDs | ChunkStore | `x, y, z` (meta-layer) |

---

# Anti-Patterns

## ❌ Storing Inventory Data in ChunkStore

**Forbidden:** Put any inventory data into ChunkStore.

ChunkStore is designed for block data only. Adding inventory fields bloats the chunk format and requires inventories to be serialized into every chunk. The chunk becomes 10× larger with no benefit.

When you write a block with an inventory, the inventory belongs to the TileEntity — not the block. Write to EntityStateStore instead.

## ❌ Saving Player Inventory to MetaDB on World Save

**Forbidden:** Write player inventory to MetaDB when saving a world.

MetaDB is player-bound. It persists independently. If a player disconnects and the world saves, writing their inventory to MetaDB on every world save is wasteful. MetaDB already knows the inventory.

Only write to MetaDB when the player logs out.

## ❌ Using ChunkStore to Store TileEntity State

**Forbidden:** Put TileEntity state in ChunkStore.

ChunkStore uses block coordinates as keys. TileEntity state changes frequently — machines update every tick. ChunkStore is not optimized for this workload.

Use EntityStateStore, which is designed for coordinate-keyed state with high update frequency.

---

# Rationale

## Chunk Is Immutable Block Dump

A chunk is a snapshot of what blocks exist at coordinates. It never changes. When you load a chunk, you read a flat array of 32×32×32 blocks. Each block is an opaque data structure: ID, metadata, and multiblock ID (if any).

ChunkStore never stores inventories because inventories are not part of the block. A block with an inventory — furnace, chest, or workbench — still contains the same block data. The inventory is separate.

## TileEntity State Changes Frequently

Machines update every tick. Furnace fuel drops. Workbench contents shift as items are crafted. Chests change as players take and place items. These updates require fast, optimized reads and writes.

ChunkStore is optimized for block data — the hot path for terrain rendering and block updates. Adding inventory writes to ChunkStore would corrupt this optimization. The store would slow down because it handles far more than block data.

## EntityStateStore Is Designed for This

EntityStateStore uses LMDB with coordinate keys. Each TileEntity has its own entry. Reads and writes are fast. The store is optimized for the exact workload: frequent updates at known coordinates.

---

# File Locations

## Epic Files

| File | Purpose |
|------|---------|
| `doc/EPICS/0-foundation-item-inventory/0-foundation-item-inventory.md` | Epic specification |
| `doc/EPICS/0-foundation-item-inventory/1-meta-db.md` | MetaDB implementation |
| `doc/EPICS/0-foundation-item-inventory/2-entity-state-store.md` | EntityStateStore implementation |
| `doc/EPICS/0-foundation-item-inventory/3-chunk-store.md` | ChunkStore implementation |

## Implementation Files

| File | Service | Notes |
|------|---------|-------|
| `src/services/meta_db/` | Go | SQLite, player data |
| `src/services/entity_state_store/` | C++ | LMDB, TileEntity state |
| `src/services/chunk_store/` | C++ | LMDB, block data |

## Protocol Files

Inventory data flows through the binary protocol:

- `PlayerInventorySnapshot` — full inventory on login
- `TileEntitySnapshot` — full entity state on world load
- `BlockUpdate` — block changes, never inventory

---

# Acceptance Criteria

## Scenario: Player Joins and Gets Inventory

Given a player has logged out with items in their inventory

When the player logs back in

Then MetaDB returns the full inventory snapshot

And the inventory appears in the client

And ChunkStore is never queried for inventory data

## Scenario: Furnace Contents Change

Given a furnace is running at coordinates (100, 64, 200)

When the furnace consumes fuel and advances the recipe

Then EntityStateStore writes the new contents

And the client receives the updated TileEntity snapshot

And ChunkStore remains unchanged (it never stores furnace contents)

## Scenario: Player Places a Chest

Given a player places a chest at coordinates (50, 65, 100)

When the player breaks the chest

Then EntityStateStore writes the chest contents before the block breaks

And the client receives a TileEntitySnapshot with the contents

And ChunkStore only records the block type change (from chest to air)

## Scenario: World Save

Given a world with multiple entities and a player

When the world saves

Then MetaDB persists the player's inventory

And EntityStateStore persists all TileEntity state

And ChunkStore persists only block and meta-layer data

## Scenario: Chunk Store Query

Given a ChunkStore query for block data at coordinates (10, 10, 10)

When the query asks for inventory data

Then the query returns an error

And the error message states that inventory data is not stored in ChunkStore

---

# Notes

- Inventory data is never serialized into FlatBuffers chunks.
- ChunkStore never has an inventory field in its schema.
- TileEntity state is completely separate from block state.
- MetaDB is the only store that persists data beyond the world lifecycle.