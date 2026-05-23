# TASK: EntityStateStore — Machine/World Entity Persistence
**Layer**: 1
**Status**: MVP Complete (merged into MetaDB)
**Epic**: 1-gameplay-machines-multiblocks

## Affected Services

| Service | Role | R/W |
|---------|------|-----|
| **MetaDB** ⬅️ (Go/SQLite, MVP) | Primary — dumb TileEntity KV store | Read/Write |
| **tile_entity_store.fbs** ⬅️ NEW | Schema for machine TileEntity state RPC | — |
| **SimulationCore** | Consumer — saves/restores machine state on chunk load/unload | Read/Write |
| **Gateway** | Relay — serves TileEntity state to clients | Read |
| **ChunkStore** | Independent — never stores entity state | — |
| **MessageRouter** | Transport — pub/sub for state changes | — |

> **⚠️ MVP architecture**: EntityStateStore is **NOT a separate C++/LMDB service**. It runs inside MetaDB (Go/SQLite) at `entity_state_handler.go`. LMDB deferred to post-MVP (>1000 writes/tick).
>
> **Schema split**: `entitystatestore.fbs` → `meta_db.fbs` (player inventory) + `tile_entity_store.fbs` (machine TileEntity state).

## Overview

Entity state persistence for machines, workbenches, and other TileEntities. On MVP it runs inside MetaDB (Go/SQLite) as `entity_state_handler.go` — a dumb key-value store that knows nothing about machine logic.

## API (MVP — Go/SQLite via MetaDB)

### GetEntityState

```go
func (m *MetaDB) GetEntityState(dim, x, y, z int) ([]byte, error)
```

Returns serialized blob for the entity at given coordinates. Returns `nil, sql.ErrNoRows` if no state stored.

### SetEntityState

```go
func (m *MetaDB) SetEntityState(dim, x, y, z int, blob []byte) error
```

Stores or overwrites serialized blob. Uses SQLite upsert (`INSERT ... ON CONFLICT DO UPDATE`).

### DeleteEntityState

```go
func (m *MetaDB) DeleteEntityState(dim, x, y, z int) error
```

Removes stored blob. Returns error if no state existed at that position.

## Data Model (MVP — SQLite)

SQLite composite primary key: `(dim INTEGER, x INTEGER, y INTEGER, z INTEGER)`. Blob is opaque `BLOB` — MetaDB never interprets it.

## Implementation Approach (MVP)

1. **SQLite table** `entity_state(dim, x, y, z, blob)` with upsert semantics
2. **No separate service** — runs inside MetaDB (Go), reuses existing DB connection
3. **RPC transport**: direct TCP `:5006` + MessageRouter topics `meta_db.entity.*`
4. **Blob format**: FlatBuffers `MachineState`/`MachineInventory` from `tile_entity_store.fbs` (defined per machine type by SimulationCore)
5. **No garbage collection on MVP** — orphaned blobs removed via explicit `DeleteEntityState`

## Storage Backend

### MVP: SQLite (via MetaDB)

MetaDB already runs SQLite for player inventory. Adding an `entity_state` table adds zero operational complexity.

### Post-MVP: LMDB

LMDB planned for Layer 2 when simulation tick rate exceeds 1000 writes/second. Read-optimized, mmap-backed, zero-copy.

## File Locations (MVP)

| File | Purpose |
|------|---------|
| `src/services/meta_db/entity_state_handler.go` | SQLite CRUD for entity state |
| `src/services/meta_db/flatbuffer_tcp.go` | Direct TCP RPC handler (`:5006`) |
| `src/services/meta_db/router_client.go` | MessageRouter integration |
| `src/protocol/tile_entity_store.fbs` | Machine TileEntity state RPC schema |
| `src/protocol/meta_db.fbs` | Player inventory RPC schema |

## Acceptance Criteria

#### Scenario: Retrieve furnace inventory after world reload

1. Player places furnace at X=100, Y=64, Z=200 in dimension 0
2. Player adds coal and ore to input slots
3. Server reloads chunk
4. `GetState` returns non-empty blob containing coal and ore in inventory
5. SimulationCore restores machine state from blob

#### Scenario: Store workbench crafting state

1. Player opens workbench GUI at X=50, Y=60, Z=100
2. Player crafts item and closes GUI
3. Server stores crafting result in state blob
4. Player reopens GUI — crafting result persists
5. `GetState` returns blob with crafting result

#### Scenario: Delete entity state on block removal

1. Machine at X=10, Y=70, Z=50 has saved state
2. Player breaks the block
3. SimulationCore removes multiblock controller and deletes state blob
4. `GetState` returns empty blob
5. No orphaned data remains

#### Scenario: Concurrent state writes from multiple threads

1. Thread A calls `SetState` for machine at X=100, Y=64, Z=200
2. Thread B calls `SetState` for machine at X=101, Y=64, Z=200
3. Both writes complete without corruption
4. Each machine retains correct state
5. No deadlock or race condition detected