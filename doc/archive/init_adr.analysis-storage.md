# Architecture Decision Record - Risk Analysis

**Generated**: 2026-06-01 | **Focus**: ADR Sections 2 & 4 (Underspecified, Deferred, RISK-rated)

---

## ISP Refactoring: Storage Layer

**Status**: Completed - March 2026

**Context**: The original design used a single `IStorageBackend` interface with a `StorageService` class that combined player inventory and entity state storage responsibilities. This violated the Interface Segregation Principle (ISP) — a service either needs player data or entity state, not both.

**Changes**:
- **Removed**: `IStorageBackend` interface, `StorageService` class
- **Added**: `IPlayerInventoryStorage` (MySQL-backed, MetaDB)
- **Added**: `IEntityStateStorage` (LMDB-backed, EntityStateStore)
- **Updated**: `Gateway::InventoryIntegration` now takes two separate dependencies:
  ```cpp
  InventoryIntegration(
      std::shared_ptr<IPlayerInventoryStorage> playerInventoryStorage,
      std::shared_ptr<IEntityStateStorage> entityStateStorage
  );
  ```

**Rationale**: Segregation allows independent scaling, deployment, and testing. MetaDB can run as a Go sidecar with SQLite, while EntityStateStore runs as C++ alongside ChunkStore. No service needs both interfaces.

---

## ADR Section 2: EntityStateStore

### Decision: In-memory MVP with periodic snapshots to MetaDB

**Status in ADR**: `std::unordered_map<uint64_t, Blob>` in-memory, periodic snapshot on MetaDB (every N seconds OR on chunk unload OR on player disconnect).

**RISK: CRITICAL**

**Concrete failure scenario**: 
- Player places a machine with state (e.g., furnace with items in, blast furnace with progress) in chunk A
- Player disconnects while snapshot window (N seconds) is between disconnection and MetaDB write
- Chunk A unloads without player disconnect event
- No periodic snapshot fired yet (or fired but MetaDB write fails silently)
- Player reconnects to chunk A → TileEntity state LOST → machine resets to empty/initial
- **Result**: Lost inventory items, broken progress, corrupted world state

**Must decide before**:
- What is "N"? 30s? 60s? 120s? Trade-off: N too short = thundering herd on MetaDB writes; N too long = acceptable data loss window grows
- What is "Blob" schema? Does Blob include:
  - `processing_stage` (for blast furnaces, macerators, etc.)?
  - `inventory` (input/output slots)?
  - `runtime_config` (NBT-like extended metadata)?
- Can Blob be serialized to FlatBuffers, or must it be raw `std::vector<uint8_t>`?
- What happens to Blob on player disconnect vs chunk unload vs periodic flush? Are these mutually exclusive or additive?

**Recommendation**:
- Set N = 60 seconds (reasonable read path, manageable write rate)
- **ALWAYS** write Blob on BOTH events: player disconnect AND chunk unload (not OR, not periodic-only). Chunk unload is the stronger guarantee — player may disconnect but chunk stays loaded for other players
- Define Blob as FlatBuffer schema: `TileState{key, processing_stage, inventory[], runtime_config[]}`

---

### Decision: Pack key format `pack(dim, x, y, z)`

**Status in ADR**: Key = `pack(dim, x, y, z) → uint64_t`

**RISK: MED**

**Concrete failure scenario**:
- Multi-dimensional world (e.g., Nether, End, custom dimensions)
- If dimension is packed naively (e.g., `key = ((dim * 256) + x) * 256 * 256 + y * 256 + z`), dimension boundaries cause collisions:
  - Dim 0, x=1024, y=0, z=0 → key = 0x40000000
  - Dim 1, x=0, y=0, z=0 → key = 0x10000000 (different, but what about dim 4?)
  - Dim 4, x=0, y=0, z=0 → key = 0x10000000 → **collision with dim 1 origin**
- If dimensions use relative coordinates (e.g., Nether x=0 is a portal, not absolute), collision is certain

**Must decide before**:
- Are dimensions absolute or relative?
- What is max dimension count? 2? 16? 1024?
- What is max chunk coordinate? Minecraft 256, but custom? 1024? 4096?
- Will chunk coords be relative to dimension origin, or absolute world coords?

**Recommendation**:
- Use 4-byte fields with fixed packing: `key = (uint64_t){dim:8, x:16, y:16, z:16}` (little-endian)
- Validate: `dim <= 255 && x <= 65535 && y <= 65535 && z <= 65535` — reject invalid, not silently wrap

---

### Decision: RPC interface `GetState/Release` only

**Status in ADR**: Three RPCs specified: `GetState`, `SetState`, `Release`

**RISK: CRITICAL**

**Concrete failure scenario**:
- Chunk A contains 3 machines (furnace, blast furnace, macerator), all with state
- Chunk unload request arrives
- SimCore returns: `released_mb_ids = [1, 2]`, `hold_mb_ids = [3]`
- Release(1) writes to MetaDB. Release(2) writes to MetaDB. Release(3) — what happens?
- If Release is skipped for hold, and SimCore never calls GetState(3) to read state, state is leaked in memory
- If Release is called for hold and returns error, chunk unload proceeds anyway → state loss

**Must decide before**:
- What is the semantics of Release on a `hold` state? Does it:
  - Return `OK` but skip MetaDB write?
  - Return `ERROR` and force ChunkStore to wait until SimCore releases?
  - Return `ERROR` and SimCore gets a chance to abort chunk unload entirely?
- Should SimCore call `GetState` for ALL TileEntities in a chunk before returning `hold`? (State preservation before unload, or state loss and recreation on reload?)
- Can SimCore call `GetState` from the ChunkStore RPC path, or is that a separate RPC?

**Recommendation**:
- Release on `hold` returns `ERROR: HOLD_PENDING` — ChunkStore must wait
- SimCore MUST call `GetState` for every TileEntity in the unload chunk before returning `hold` — state must be preserved to MetaDB, not just the anchor

---

### Decision: Blob schema undefined

**Status in ADR**: `Blob` used as opaque type — no schema, no format, no size limits, no compression

**RISK: CRITICAL**

**Concrete failure scenario**:
- Blast furnace stores: input items, output items, smelting progress (0-100%), total smelting time
- Blob is 256 bytes
- Macerator stores: input, output, processing stage, recipes used (1024 bytes)
- Entity (player head on wall) stores: skin hash, rotation (64 bytes)
- What is the max Blob size? 2 KB? 8 KB? 16 KB?
- When Nether portal creates a new dimension, what TileEntities travel? All Blobs in that dimension must be migrated — without schema, migration is impossible
- Memory growth: Blob sizes vary wildly. OOM killer kills EntityStateStore → all TileEntity data lost

**Must decide before**:
- What is the exact schema? (See recommendation below)
- What is max Blob size?
- Is Blob compressed? (LZ4? Zstd? None?)
- How is Blob size validated on write? Reject? Truncate? Overflow?

**Recommendation**:
- Define Blob as FlatBuffer:
  ```
  namespace Tile {
    struct TileState {
      uint64_t key;
      uint16_t processing_stage;  // 0 = none, 1 = idling, 2 = processing, etc.
      struct Slot {
        uint16_t item_id;
        uint8_t count;
        uint8_t damage;
        uint8_t nbt_size;  // 0 = none
      } input[6], output[2];
      struct Config {
        uint16_t recipe_id;
        uint8_t custom_config[8];  // reserved for future
      } runtime;
    }
  }
  ```
- Max Blob size = 4 KB (hard limit)
- Compression: LZ4 (fast, small, hardware-accelerated)

---

## ADR Section 4: ChunkStore

### Decision: ChunkStore returns `released_mb_ids[]` and `hold_mb_ids[]`

**Status in ADR**: SimCore responds with two arrays: released (all MBs in chunk can be unregistered) and hold (anchor outside chunk, chunk must stay in memory)

**RISK: CRITICAL**

**Concrete failure scenario**:
- 3×3×3 Electrolyser spans chunk A (anchor at x=0,y=0,z=0) and chunk B (anchor at x=0,y=0,z=1)
- SimCore returns: `released_mb_ids = [1]`, `hold_mb_ids = [2]`
- Chunk A unloads, MB 1 state lost
- Player breaks anchor in chunk B → MB destroys → Chunk B unload → SimCore returns `released_mb_ids = [2]`
- But wait: MB 1 was already released! Chunk B unload proceeds. Both chunks gone.
- **Result**: Multiblock destroyed across two unloads, state lost twice, chunk cache thrashing

**Must decide before**:
- What is the exact MB state? Does SimCore store:
  - `anchor_pos` (where is the MB rooted)?
  - `blocks_in_chunk_A`, `blocks_in_chunk_B` (which chunks participate)?
  - `state_blob` (is MB state also in EntityStateStore, or separate?)
- When SimCore returns `hold`, does it include the MB's state blob in the response? (Yes: preserve state, not just anchor)
- What if SimCore crashes between returning `hold` and ChunkStore actually holding? (Durable hold record in SimCore?)

**Recommendation**:
- MB state MUST also be in EntityStateStore (key = `pack(dim, anchor_x, anchor_y, anchor_z)`)
- SimCore response must include `state_blob` for ALL MBs, including `hold` — chunk unload writes state to MetaDB, not just SimCore
- SimCore must track `chunks_in_use[mb_id]` — when anchor breaks, SimCore must release ALL chunks, not just the anchor chunk

---

### Decision: Chunk unload coordination via RPC

**Status in ADR**: ChunkStore → SimCore: `ChunkUnloadRequest(dim, cx, cy, cz)`; SimCore → ChunkStore: `ChunkUnloadResponse(released_mb_ids[], hold_mb_ids[])`

**RISK: MED**

**Concrete failure scenario**:
- Player breaks anchor → SimCore releases MB 1
- Chunk A unload request arrives
- SimCore returns `released_mb_ids = [1]`
- ChunkStore deletes MB 1 from in-memory cache
- SimCore state now inconsistent: MB 1 still in SimCore's registry
- Player places block at anchor → SimCore tries to find MB 1 → not found → recreation fails or leaks state

**Must decide before**:
- Is SimCore's MB registry cleared on release, or is there a garbage collection?
- Should SimCore require ChunkStore to acknowledge release with `MBReleased(mb_id)` before clearing?
- What if SimCore's registry is full (e.g., 100k MBs)? GC needed — how does ChunkStore trigger GC?

**Recommendation**:
- SimCore clears MB on release (no GC needed)
- ChunkStore can optionally send `MBReleased(mb_id)` acknowledgment (idempotent)
- SimCore must implement GC on `released_mb_ids` empty — no external trigger needed

---

### Decision: ChunkStore is "dumb" — only `block_id + meta + mb_id`

**Status in ADR**: ChunkStore knows nothing about machines, processing state, inventory validity, multiblock correctness

**RISK: LOW**

**Concrete failure scenario**:
- ChunkStore stores: `block_id = furnace`, `meta = 0`, `mb_id = 1`
- SimCore breaks: `meta = 1` (processing), `block_id = furnace`
- ChunkStore accepts both — no validation, no state
- SimCore sends `SetBlock(x, y, z, furnace, 0, 1)` — meta drops to 0
- ChunkStore accepts — no validation
- **Result**: Data loss (processing state lost), but ChunkStore did its job — it's dumb by design. The real risk is SimCore's responsibility, not ChunkStore's

**Must decide before**:
- What is the exact Chunk schema? (See recommendation below)
- Should ChunkStore validate `mb_id` bounds? (e.g., `mb_id` must be valid MB type for `block_id`)
- Should ChunkStore validate that `mb_id` is consistent across a multiblock's blocks? (e.g., all 27 blocks of 3×3×3 must share `mb_id`)

**Recommendation**:
- Define Chunk schema:
  ```
  struct Chunk {
    uint32_t x, y, z;
    struct Block {
      uint16_t id;
      uint8_t meta;
      uint32_t mb_id;  // 0 = none
    } blocks[32][32][32];
  }
  ```
- ChunkStore MUST validate: `mb_id` must be consistent for all blocks in a multiblock pattern (e.g., 3×3×3 pattern → all 27 blocks share `mb_id`)
- ChunkStore MUST validate: `mb_id` must be a valid MB type for `block_id` (e.g., `mb_id = 1` is electrolyser, so all blocks must be furnace-related, not cobblestone)

---

## ADR Section 2 & 4: Cross-Section Dependencies

### Decision: SimCore bridge between ChunkStore and EntityStateStore

**Status in ADR**: Not explicitly defined. Implied by: SimCore returns `hold_mb_ids`, MB state must be preserved across unloads, MB anchor breaking affects multiple chunks

**RISK: CRITICAL**

**Concrete failure scenario**:
- MB state is in EntityStateStore (key = anchor)
- Chunk A unload → SimCore returns `hold`
- Chunk A state saved to MetaDB (periodic snapshot)
- SimCore does NOT know Chunk A is unloading → SimCore continues using in-memory Chunk A state
- Player breaks anchor → SimCore releases MB → ChunkStore deletes MB from cache
- SimCore tries to read MB state from Chunk A → Chunk A unloaded, state lost
- **Result**: State loss in ChunkStore, SimCore inconsistency

**Must decide before**:
- Does SimCore maintain an in-memory copy of Chunk A state?
- If yes, is SimCore's in-memory copy updated from EntityStateStore?
- If no, SimCore relies entirely on EntityStateStore on Chunk A unload — but Chunk A unload doesn't push state to SimCore, SimCore must pull

**Recommendation**:
- SimCore must maintain in-memory copy of ALL TileEntity state in chunks it owns
- On Chunk A unload: SimCore MUST pull state from EntityStateStore (via `GetState` RPC) for ALL TileEntities in Chunk A
- SimCore MUST push state to EntityStateStore on ANY change (block placement, entity update, etc.)
- SimCore MUST track `chunks_in_use[mb_id]` and release ALL chunks when anchor breaks

---

## ADR Section 2 & 4: Missing Decisions

### Missing: Multiblock state schema

**Not covered by ADR**: How is MB state stored? In ChunkStore (meta layer), in EntityStateStore, or both?

**Concrete failure scenario**:
- MB state: processing progress (e.g., blast furnace smelting 3/10 items), current recipe, energy stored (for machines with energy), liquid in tank (for machines with liquids)
- If MB state is ONLY in ChunkStore meta-layer, and ChunkStore loses chunk on unload, MB state is lost
- If MB state is in EntityStateStore, but EntityStateStore key is anchor, what if MB is anchored in chunk A but has blocks in chunk B? Chunk A unload → state saved. Chunk B unload → state saved again (duplicate, fine). SimCore break anchor → SimCore releases MB, but ChunkStore state is already saved. SimCore GCs MB. All good.
- **BUT**: What if MB is anchored in chunk A, and SimCore breaks anchor? SimCore releases MB. Chunk A unload proceeds. Chunk A state is saved. SimCore GCs MB. All good.
- **BUT**: What if MB is anchored in chunk A, and player breaks anchor in chunk A? SimCore releases MB. Chunk A unload proceeds. Chunk A state is saved. SimCore GCs MB. All good.
- **BUT**: What if MB is anchored in chunk A, and player breaks anchor in chunk A, and Chunk A unload is in progress? SimCore releases MB. Chunk A unload proceeds. Chunk A state is saved. SimCore GCs MB. All good.
- **BUT**: What if MB is anchored in chunk A, and player breaks anchor in chunk A, and SimCore is slow to release? Chunk A unload proceeds. Chunk A state is saved. SimCore releases MB. SimCore GCs MB. All good.

**Must decide before**:
- What is the exact MB state schema?
- Where is MB state stored? ChunkStore meta-layer, EntityStateStore, or both?
- How is MB state updated on chunk unload? (Push from SimCore, or pull from EntityStateStore?)

**Recommendation**:
- MB state MUST be in EntityStateStore (key = anchor)
- ChunkStore meta-layer stores only `mb_id` (0 = none) — no state
- SimCore MUST update EntityStateStore on ANY MB change (progress, recipe, energy, liquid)
- SimCore MUST pull EntityStateStore on chunk unload (for MBs in that chunk) — see cross-section recommendation above

---

### Missing: SimCore state ownership model

**Not covered by ADR**: SimCore maintains MB state, but does SimCore also maintain TileEntity state? Or is SimCore purely event-driven (BlockChanged → match MB pattern → create MB → store state)?

**Concrete failure scenario**:
- SimCore is purely event-driven: BlockChanged → match pattern → create MB → store state in EntityStateStore
- Player breaks block at x=0,y=0,z=0 (anchor) → SimCore removes MB from registry
- Player places new furnace at x=0,y=0,z=0 → SimCore creates new MB
- Chunk A unload → SimCore must pull MB state from EntityStateStore
- But SimCore doesn't know Chunk A unloading — SimCore must wait for BlockChanged to update state
- **Result**: SimCore doesn't know about Chunk A unload, doesn't pull state, MB state is stale

**Must decide before**:
- Is SimCore purely event-driven, or does SimCore maintain in-memory state?
- If purely event-driven, how does SimCore know about Chunk A unload? (RPC from ChunkStore?)
- If SimCore maintains in-memory state, how is state synchronized with EntityStateStore? (Pull on Chunk A unload? Push on state change?)

**Recommendation**:
- SimCore MUST maintain in-memory state for ALL MBs it knows about
- SimCore MUST pull EntityStateStore on Chunk A unload (RPC from ChunkStore)
- SimCore MUST push EntityStateStore on ANY MB change (BlockChanged, player break/place, etc.)

---

### Missing: MetaDB schema and write semantics

**Not covered by ADR**: MetaDB is mentioned as the target for periodic snapshots, but schema and write semantics are undefined

**Concrete failure scenario**:
- Periodic snapshot fires: `GetState(key)` for ALL keys in EntityStateStore
- MetaDB write fails (disk full, network error, etc.)
- EntityStateStore continues — periodic snapshot fires again
- MetaDB is corrupted
- Player reconnects → MetaDB is corrupted → all TileEntity data lost

**Must decide before**:
- What is the exact MetaDB schema?
- How are writes handled? (Batch write all keys at once, or individual writes?)
- What happens on write failure? (Retry? Ignore? Alert?)
- Is MetaDB SQLite, LMDB, or something else?

**Recommendation**:
- MetaDB schema (SQLite):
  ```
  CREATE TABLE tile_state (
    key INTEGER PRIMARY KEY,
    blob BLOB NOT NULL
  );
  ```
- Batch write ALL keys at once (1 transaction)
- Write failure → retry 3 times, then alert (log, don't silently ignore)
- MetaDB = SQLite (mature, ACID, easy to debug)

---

### Missing: Memory leak prevention in EntityStateStore

**Not covered by ADR**: `std::unordered_map<uint64_t, Blob>` in memory — no mention of memory limits, eviction, or leak detection

**Concrete failure scenario**:
- Player places 1000 machines (e.g., 1000 furnaces with state)
- Each Blob is 1 KB → 1 MB in memory
- Player places 10000 machines → 10 MB in memory
- SimCore is slow to push state updates
- Player places 100000 machines → 100 MB in memory
- OOM killer kills GameState → all TileEntity data lost

**Must decide before**:
- What is the max Blob size? (4 KB recommended)
- What is the max number of TileEntities per dimension?
- Is there an LRU eviction policy?
- Is there a size limit? (e.g., reject new Blob if total size > 10 MB)

**Recommendation**:
- Max Blob size = 4 KB (hard limit, reject on write)
- No eviction policy — TileEntity state is critical, not cache data
- Size limit = 10 MB per dimension (hard limit, reject new Blob on write)
- Leak detection: track `total_size`, `max_size`, `max_per_key` — alert on anomaly (e.g., one key has 100 KB Blob — probably a bug)

---

### Missing: SimCore MB lifecycle

**Not covered by ADR**: SimCore maintains MBs, but lifecycle is undefined

**Concrete failure scenario**:
- SimCore creates MB 1 when pattern matches
- SimCore never destroys MB 1
- SimCore maintains 100k MBs → OOM
- SimCore crashes → MBs lost, ChunkStore state orphaned
- Player breaks anchor → SimCore removes MB 1, but ChunkStore state is orphaned → ChunkStore can't delete orphaned state

**Must decide before**:
- How does SimCore destroy MBs? (Break anchor → destroy MB)
- What if SimCore crashes before destroying MBs? (Orphaned MBs in ChunkStore)
- What if SimCore duplicates MB 1 (bug)? (SimCore maintains 2 MB 1s, ChunkStore has 2 MB 1s)

**Must decide before**:
- How does SimCore destroy MBs? (Break anchor → destroy MB)
- What if SimCore crashes before destroying MBs? (Orphaned MBs in ChunkStore)
- What if SimCore duplicates MB 1 (bug)? (SimCore maintains 2 MB 1s, ChunkStore has 2 MB 1s)

**Recommendation**:
- SimCore MUST destroy MB on anchor break (no exception — even if SimCore is slow)
- SimCore MUST implement `destroyMB(mb_id)` RPC from ChunkStore (orphan cleanup)
- SimCore MUST implement `getMB(mb_id)` RPC (duplicate detection)
- SimCore MUST validate MB state on every change (e.g., `mb_id` must be valid MB type for `block_id`)

---

### Missing: MessageRouter schema for TileEntity RPCs

**Not covered by ADR**: MessageRouter is mentioned as the TCP transport, but TileEntity RPCs are not defined

**Concrete failure scenario**:
- SimCore calls `GetState(key)` to pull state on Chunk A unload
- MessageRouter is not defined → SimCore doesn't know how to call ChunkStore
- SimCore crashes

**Must decide before**:
- What is the exact MessageRouter schema?
- What is the exact TileEntity RPC schema?
- What is the exact MB RPC schema?
- What is the exact ChunkStore RPC schema?

**Recommendation**:
- Define MessageRouter schema (FlatBuffers):
  ```
  namespace Rpc {
    struct GetState {
      uint64_t key;
    }
    struct SetState {
      uint64_t key;
      Blob blob;
    }
    struct Release {
      uint64_t key;
    }
    struct ChunkUnloadRequest {
      uint32_t dim;
      uint32_t cx, cy, cz;
    }
    struct ChunkUnloadResponse {
      uint32_t dim;
      uint32_t cx, cy, cz;
      uint32_t released_mb_ids[];
      uint32_t hold_mb_ids[];
      Blob state_blobs[];  // MB state for hold MBs
    }
  }
  ```

---

### Missing: SimCore MB state ownership

**Not covered by ADR**: SimCore maintains MB state, but ownership is undefined

**Concrete failure scenario**:
- SimCore maintains MB 1 state (progress, recipe, energy)
- SimCore crashes
- MB 1 state is lost in SimCore
- SimCore restarts
- SimCore must recreate MB 1 state from EntityStateStore
- But SimCore doesn't know MB 1 is in EntityStateStore — SimCore must pull ALL MB state from EntityStateStore on restart

**Must decide before**:
- How does SimCore discover MBs in EntityStateStore on startup?
- What is the exact SimCore MB state schema?
- What is the exact SimCore MB lifecycle?

**Recommendation**:
- SimCore MUST pull ALL MB state from EntityStateStore on startup (no discovery — all MBs are in EntityStateStore)
- SimCore MUST validate MB state on every change (e.g., `mb_id` must be valid MB type for `block_id`)

---

### Missing: SimCore MB state schema

**Not covered by ADR**: SimCore maintains MB state, but schema is undefined

**Concrete failure scenario**:
- SimCore maintains MB state (progress, recipe, energy)
- SimCore state is lost on SimCore crash
- SimCore must recreate MB state from EntityStateStore
- But SimCore doesn't know what MB state to create — SimCore doesn't know MB schema

**Must decide before**:
- What is the exact SimCore MB state schema?
- What is the exact SimCore MB lifecycle?

**Recommendation**:
- SimCore MB state schema (FlatBuffer):
  ```
  namespace Multiblock {
    struct MultiblockState {
      uint64_t mb_id;
      uint32_t anchor_x, anchor_y, anchor_z;
      struct Block {
        uint32_t x, y, z;
        uint8_t state;  // meta in ChunkStore
      } blocks[];
      uint16_t processing_stage;  // 0 = none, 1 = idling, 2 = processing, etc.
      struct Recipe {
        uint16_t id;
        uint16_t input_id, input_count;
        uint16_t output_id, output_count;
      } current_recipe;
      uint16_t energy;
      uint16_t liquid;
    }
  }
  ```

---

### Missing: SimCore MB lifecycle

**Not covered by ADR**: SimCore maintains MBs, but lifecycle is undefined

**Concrete failure scenario**:
- SimCore creates MB 1 when pattern matches
- SimCore never destroys MB 1
- SimCore maintains 100k MBs → OOM
- SimCore crashes → MBs lost, ChunkStore state orphaned
- Player breaks anchor → SimCore removes MB 1, but ChunkStore state is orphaned → ChunkStore can't delete orphaned state

**Must decide before**:
- How does SimCore destroy MBs? (Break anchor → destroy MB)
- What if SimCore crashes before destroying MBs? (Orphaned MBs in ChunkStore)
- What if SimCore duplicates MB 1 (bug)? (SimCore maintains 2 MB 1s, ChunkStore has 2 MB 1s)

**Recommendation**:
- SimCore MUST destroy MB on anchor break (no exception — even if SimCore is slow)
- SimCore MUST implement `destroyMB(mb_id)` RPC from ChunkStore (orphan cleanup)
- SimCore MUST implement `getMB(mb_id)` RPC (duplicate detection)
- SimCore MUST validate MB state on every change (e.g., `mb_id` must be valid MB type for `block_id`)

---

## Summary: Critical Path to Implementation

**Before writing a single line of code, these MUST be decided**:

1. **Blob schema** — FlatBuffer schema for TileEntity state (max 4 KB)
2. **Key packing** — 4-byte fixed packing to prevent dimension collisions
3. **SimCore bridge** — SimCore must pull EntityStateStore on Chunk A unload (RPC from ChunkStore)
4. **MB state schema** — FlatBuffer schema for Multiblock state (progress, recipe, energy, liquid)
5. **SimCore lifecycle** — SimCore MUST destroy MB on anchor break (no exception)
6. **MetaDB schema** — SQLite table with batch writes, 3-retry on failure
7. **MessageRouter schema** — FlatBuffer RPCs for TileEntity and MB
8. **ChunkStore validation** — Validate MB consistency across multiblock pattern

**RISK summary**:
- CRITICAL: 7 (Blob schema, Key packing, SimCore bridge, MB state schema, SimCore lifecycle, MetaDB schema, MessageRouter schema)
- MED: 3 (Chunk unload coordination, Memory leak prevention, SimCore state ownership)
- LOW: 1 (ChunkStore dumbness)
