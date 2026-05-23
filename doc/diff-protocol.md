# Diff Protocol — Client↔Server Block Synchronisation

> **Статус**: Частично реализован. `BlockAck` type_id=5 существует в протоколе и парсится клиентом (OnBlockAck). `BlockUpdate` type_id=4 существует. Chunk version и pendingChanges_ rebase не реализованы — сейчас все изменения идут через полные ChunkData.

## Problem

Currently the server sends **full `ChunkData` snapshots** (192 KB each, all 32³ blocks)
for every change. This causes two issues:

1. **Race condition:** a client that placed a block but hasn't received the server's
   `BlockAck` yet will have its local change overwritten if the server sends a full
   `ChunkData` for any reason (other player's action, resync, etc.).
2. **Bandwidth waste:** a single block change triggers a 192 KB transfer instead of a
   few bytes.

## Proposed Solution — Incremental Diffs

### New Protocol Messages (FlatBuffers)

Add two new tables to `core.fbs`:

```flatbuffers
// Incremental block change (gateway → client).
// Sent instead of full ChunkData for single-block mutations on already-loaded chunks.
table BlockUpdate {
  pos:Vec3i (required);        // global block position
  block_id:uint16;
  meta:uint8;
  mb_id:uint32;
  chunk_version:uint64;        // chunk revision AFTER this change
}

// Server confirms that the client's PlayerAction for this block has been committed.
table BlockAck {
  pos:Vec3i (required);
}
```

Add to `GatewayPayload` union in `gateway.fbs`:

```flatbuffers
union GatewayPayload {
  PlayerAction,       // 0 — client → gateway
  ChunkData,          // 1 — gateway → client (full chunk, first load / resync)
  EntitySnapshot,     // 2 — gateway → client
  BlockUpdate,        // 3 — gateway → client (incremental)
  BlockAck,           // 4 — gateway → client (commit confirmation)
}
```

### Chunk Version

The `ChunkData` table gains an optional `version:uint64` field. The server maintains
a monotonically increasing version counter per chunk. The version increments on every
block mutation.

- `ChunkData` includes the version so the client knows from which baseline it's working.
- `BlockUpdate` includes `chunk_version` — the version AFTER the change was applied.
- The client can ignore stale `ChunkData` if it already has a newer version.
- The client uses the version to detect that its pending change is committed
  (when `BlockUpdate` arrives with a version ≥ the version the client saw before
  sending its action).

## Client-Side State

### Pending Changes

```cpp
// Per-chunk set of blocks the client has placed but not yet confirmed by server.
// Keyed by chunk key (MakeChunkKey).
tbb::concurrent_hash_map<uint64_t,
    std::unordered_set<BlockPos>, Uint64Hash> pendingChanges_;
```

### Semantics

| Event | Action on `pendingChanges_` | Action on chunk data |
|---|---|---|
| `SendPlayerAction(PLACE, pos)` | **Add** `pos` to `pendingChanges_[chunkKey]` | Apply block optimistically (if `WORLD_OPTIMISTIC_AFTER`) |
| `ChunkData(coord)` | **Keep** `pendingChanges_` as-is | Store server chunk, then **rebase** — overlay all pending blocks for this chunk on top |
| `BlockUpdate(pos, ...)` | **Keep** `pendingChanges_` as-is | Apply server value directly (server always wins) |
| `BlockAck(pos)` | **Erase** `pos` from `pendingChanges_` | Nothing — block is already correct from previous `BlockUpdate` or rebase |

### Compile-Time Options

Defined in `World.h`:

```cpp
// #define WORLD_FAST_SEND
//   Send PlayerAction immediately on place/break, without waiting for any round-trip.
//   The block is added to pendingChanges_ simultaneously.
//   Without this flag, the client batches actions or waits for some condition.

// #define WORLD_OPTIMISTIC_AFTER
//   When a block is placed, show it locally immediately (pending change applied to
//   the visible chunk) without waiting for server confirmation.
//   Without this flag, the block appears only after BlockUpdate/ChunkData arrives.

// #define WORLD_SERVER_PRIORITY
//   When a BlockUpdate conflicts with a pending change, the server value wins.
//   The pending entry stays — it will be removed only on BlockAck.
//   Without this flag, pending changes could shadow server values (not recommended).
```

### Full `ChunkData` on Already Loaded Chunk (Resync)

When a full `ChunkData` arrives for an already loaded chunk (rare — only on resync
or initial load):

1. Replace chunk data with server snapshot.
2. **Rebase:** iterate `pendingChanges_[chunkKey]` and re-apply every entry on top.
3. This ensures no local change is lost, even if the server sent a full snapshot
   without knowing about the client's uncommitted actions.

### Flow Diagram

```
Client                                    Server
  │                                         │
  │ (1) PlayerAction(PLACE, pos, STONE)     │
  │     pending[pos] ← STONE                │
  ├─────────────────────────────────────────►│
  │                                         │  server increments chunk version
  │                                         │
  │ (2a) BlockUpdate(pos, STONE, v=42)      │  ← diff for OUR (or concurrent) change
  │◄─────────────────────────────────────────┤
  │     apply STONE to chunk (already set)   │
  │     do NOT clear pending                 │
  │                                         │
  │ (2b) BlockAck(pos)                      │  ← confirmation
  │◄─────────────────────────────────────────┤
  │     pending.erase(pos)                   │
  │                                         │
  │ —or— if full snapshot arrives mid-flight:│
  │                                         │
  │ (3) ChunkData(coord, v=41)              │  ← stale: version < baseline
  │◄─────────────────────────────────────────┤
  │     ignore (version too old)             │
  │                                         │
  │ (4) ChunkData(coord, v=42)              │  ← current snapshot
  │◄─────────────────────────────────────────┤
  │     store server chunk                   │
  │     rebase: overlay pending[pos]=STONE   │
```

## Files Touched

| File | Change |
|---|---|
| `src/protocol/core.fbs` | Add `BlockUpdate`, `BlockAck` tables. Add `version` to `ChunkData`. |
| `src/protocol/gateway.fbs` | Add `BlockUpdate`, `BlockAck` to `GatewayPayload`. |
| `src/services/chunk_store/Chunk.h` | Add `uint64_t version` field. |
| `src/services/game_client/World/World.h` | Add `pendingChanges_` field. Compile-time defines. |
| `src/services/game_client/World/World.cpp` | `OnChunkData` rebase. New `OnBlockUpdate`, `OnBlockAck`. |
| `src/services/game_client/Network/NetClient.h` | New callbacks for `BlockUpdate`, `BlockAck`. |
| `src/services/game_client/Network/NetClient.cpp` | Dispatch new message types, parse `BlockUpdate`/`BlockAck`. |
| `src/services/gateway/` | Send `BlockUpdate` instead of `ChunkData` for single mutations. Maintain chunk version. |
| `src/services/chunk_store/` | Server-side: increment version on `SetBlock`. |

## Implementation Order (When Ready)

1. FlatBuffers schema: `BlockUpdate`, `BlockAck`, `version` in `ChunkData`.
2. Regenerate generated headers.
3. `Chunk.h` — add `version` field.
4. `World.h` — add `pendingChanges_` and compile-time options.
5. `World.cpp` — implement `OnBlockUpdate`, `OnBlockAck`, rebase in `OnChunkData`.
6. `NetClient.h/.cpp` — callbacks, parsing, dispatch.
7. Server side (Gateway / ChunkStore) — send diffs, maintain version.
