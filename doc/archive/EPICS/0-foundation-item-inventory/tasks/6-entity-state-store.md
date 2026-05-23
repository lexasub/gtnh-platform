# TASK: EntityStateStore — Machine/World Inventory Persistence
**Layer**: 0
**Status**: MVP Complete (separate C++/LMDB service)
**Epic**: 0-foundation-item-inventory

## Affected Services

| Service | Role | R/W |
|---------|------|-----|
| **EntityStateStore** ⬅️ (C++/LMDB) | Primary — dumb TileEntity KV store | Read/Write |
| **tile_entity_store.fbs** ⬅️ NEW | Schema for machine TileEntity state RPC | — |
| **SimulationCore** | Consumer — reads/writes machine state on tick | Read/Write |
| **Gateway** | Relay — forwards TileEntity state to client | Read |
| **ChunkStore** | Independent — never stores entity state | — |
| **MessageRouter** | Transport — pub/sub for state changes | — |

> **Architecture**: Separate C++/LMDB service on MVP. LMDB provides mmap-backed zero-copy reads, ACID transactions, and scale to >1000 writes/tick.

## Overview

EntityStateStore persists generic TileEntity state data — machine inventories, workbench contents, chest contents. It operates as a dumb key-value store for arbitrary blobs. Unlike ChunkStore (block storage), EntityStateStore handles variable-size entity state.

**Backend**: LMDB (1GB mapsize). Key format: `(dim, x, y, z, entityType) → blob`.

## API

### GetEntityState

```cpp
bool LoadEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z,
                    uint16_t entityType, std::vector<uint8_t>& stateData);
```

Retrieves the state blob for a TileEntity at the given coordinates.

**Arguments:**
- `dimension` - dimension identifier (0 = overworld, 1 = nether, 2 = end)
- `x, y, z` - world coordinates
- `entityType` - TileEntity type ID
- Returns `true` if found, `false` otherwise

### SetEntityState

```cpp
bool SaveEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z,
                    uint16_t entityType, const std::vector<uint8_t>& stateData);
```

Stores or overwrites a state blob for a TileEntity.

### DeleteEntityState

```cpp
bool DeleteEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z,
                      uint16_t entityType);
```

Removes the state blob for a TileEntity.

## Data Model

### Value Format

The blob is an opaque byte sequence (`std::vector<uint8_t>` in C++). Consumers must know the format, which varies by TileEntity type:

| Entity Type | Format |
|-------------|--------|
| Machine | FlatBuffers `MachineState` (from `tile_entity_store.fbs`) |
| Chest/Inventory | FlatBuffers `MachineInventory` (from `tile_entity_store.fbs`) |
| Generic | Custom binary |

EntityStateStore never validates or interprets blob content. It is truly dumb storage.

## Storage Backend

### LMDB

- **Read-optimized**: TileEntity state is read frequently during simulation but written rarely
- **Mmap-backed**: Zero-copy reads from disk
- **ACID**: Transactions ensure durability and consistency
- **Embedded**: Single library dependency, no separate process
- **1GB mapsize**: Configured via `mdb_env_set_mapsize`

## Implementation Approach

1. **Service** — C++ service with LMDB backend, asio network layer, and LRU cache (1000 entries)
2. **Network** — MessageRouter topics + FlatBuffers TCP (`:5006`)
3. **Topics**:
   - `entity.state.get` → `GetEntityState`
   - `entity.state.set` → `SetEntityState`
   - `world.blocks.changed` → event listener
4. **Key format**: `dim|x|y|z|entityType`

## Difference from ChunkStore

| Aspect | ChunkStore | EntityStateStore |
|--------|------------|------------------|
| **Purpose** | Block storage, chunk snapshots | Generic TileEntity state |
| **Backend** | LMDB (C++) | LMDB (C++) |
| **Key** | Chunk coordinates (x, y, z) | Entity coordinates (dim, x, y, z, entityType) |
| **Value** | 32 KB chunk data (fixed) | Variable blob (any size) |
| **Specialization** | Knows block IDs and metadata | Knows nothing — completely generic |
| **Relationship** | Simultaneously alive (both needed) | Independent |

EntityStateStore does not understand "electrolyser", "furnace", or any machine-specific logic. It only stores and retrieves blobs.

## File Locations

| File | Purpose |
|------|---------|
| `src/services/entity_state_store/main.cpp` | Service + LMDB + asio network layer |
| `src/services/entity_state_store/cache.cpp` | LRU cache (1000 entries) |
| `src/services/entity_state_store/EntityStateStorage.h` | IEntityStateStorage interface |
| `src/services/entity_state_store/Storage/IEntityStateStorage.cpp` | LMDB implementation |
| `src/protocol/tile_entity_store.fbs` | Machine TileEntity state RPC schema |
| `src/protocol/core.fbs` | Shared types (InventorySlot, InventoryUpdate) |

## Acceptance Criteria

#### Scenario 1: Store and retrieve state

A TileEntity at dimension 0, coordinates (100, 64, 200) has its state stored and retrieved successfully.

**Given:** EntityStateStore initialized with LMDB
**When:** SaveEntityState(0, 100, 64, 200, 1, blob) is called with a 128-byte blob
**And:** LoadEntityState(0, 100, 64, 200, 1, data) is called
**Then:** The returned data matches the original 128 bytes exactly

#### Scenario 2: Overwrite state

A previously stored TileEntity state is overwritten.

**Given:** A TileEntity state exists at (0, 100, 64, 200)
**When:** SaveEntityState(0, 100, 64, 200, 1, new_blob) is called with different data
**Then:** LoadEntityState returns the new blob, not the old one

#### Scenario 3: Empty state

A TileEntity at coordinates that have never been written returns false.

**Given:** No TileEntity state stored at (1, 50, 64, 150)
**When:** LoadEntityState(1, 50, 64, 150, 1, data) is called
**Then:** false is returned, data is empty

#### Scenario 4: Delete state

A previously stored state is deleted.

**Given:** A TileEntity state exists at (0, 10, 70, 50)
**When:** DeleteEntityState(0, 10, 70, 50, 1) is called
**Then:** Delete succeeds, subsequent LoadEntityState returns false

#### Scenario 5: Delete nonexistent state

Deleting a state that doesn't exist returns false (no error).

**Given:** No TileEntity state at (2, 0, 0, 0)
**When:** DeleteEntityState(2, 0, 0, 0, 1) is called
**Then:** false is returned (not an error)

#### Scenario 6: Multiple dimensions

States are isolated by dimension.

**Given:** States stored at same coordinates (100, 64, 200) in dimensions 0 and 1
**When:** LoadEntityState(0, 100, 64, 200, 1, data) and LoadEntityState(1, 100, 64, 200, 1, data)
**Then:** Each returns the correct dimension-specific blob

#### Scenario 7: Cache hit

Repeated reads hit the LRU cache.

**Given:** A TileEntity state exists at (0, 100, 64, 200)
**When:** LoadEntityState is called twice
**Then:** Second call returns true immediately (cache hit), LMDB not touched

#### Scenario 8: Cache eviction

Cache evicts oldest entries when max_size (1000) is exceeded.

**Given:** Cache contains 1001 entries
**When:** A new entry is added
**Then:** Oldest entries are evicted, new entry is cached
