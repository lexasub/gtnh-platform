# TASK: Multiblock Foundation
**Layer**: 2
**Status**: Draft
**Epic**: 1-gameplay-machines-multiblocks

## Affected Services

| Service | Role | R/W |
|---------|------|-----|
| **SimulationCore** | Primary — pattern detection, MultiblockController ECS lifecycle | Read/Write |
| **SpatialIndex** | Dependency — R-tree queries for multiblock pattern matching | Read |
| **ChunkStore** | Dependency — block data for pattern verification, mb_id writes | Read/Write |
| **EntityStateStore** ⬅️ NEW | Persistence — multiblock controller state snapshots | Write |
| **RecipeManager** ⬅️ NEW | Dependency — multiblock recipes (e.g., blast furnace layers) | Read |
| **GameClient** | Consumer — MultiblockStatus rendering | Read |

---

# Overview

Multiblocks are unified machine entities composed of multiple blocks arranged in a fixed pattern. They represent machines with shared state — input/output slots, shared inventories, shared energy storage, and special rendering — that cannot be expressed as independent block components.

The multiblock system consists of two distinct components:

- **Pattern Detection** — SimulationCore monitors world changes and matches block patterns to known machine templates
- **Controller Lifecycle** — Once formed, a MultiblockController manages the machine's state as a single entity

## Design Philosophy

Multiblocks are **not** emergent behavior. They are explicitly defined machines with explicit patterns. The system does not infer machine identity from block placement alone — it relies on a canonical pattern registry maintained by the SimulationCore.

## Service Boundaries

```
┌─────────────────────────────────────────────────────┐
│  ChunkStore                                         │
│  ──────────────────────────────────────────────────  │
│  • Stores chunks with embedded mb_id in meta-layer  │
│  • Does not understand what multiblocks are         │
│  • Only knows block_id + meta + mb_id               │
└─────────────────────────────────────────────────────┘

        │
        │ Publishes BlockChanged on pattern match
        ▼
┌─────────────────────────────────────────────────────┐
│  SimulationCore                                     │
│  ──────────────────────────────────────────────────  │
│  • Maintains MultiblockController registry           │
│  • Detects patterns via BlockChanged subscription   │
│  • Creates/destroys controllers                      │
│  • Owns multiblock state as unified entities        │
└─────────────────────────────────────────────────────┘

        │
        │ RPC: GetBlock in pattern radius
        ▼
┌─────────────────────────────────────────────────────┐
│  ChunkStore                                         │
│  ──────────────────────────────────────────────────  │
│  • Returns block data + mb_id for each position     │
└─────────────────────────────────────────────────────┘
```

## Key Rules

- **SimulationCore owns MultiblockController** — Only SimulationCore creates, destroys, and tracks controllers. ChunkStore never creates or manages them.
- **ChunkStore stores mb_id in meta-layer** — The multiblock ID is embedded in each block's metadata within the chunk data. ChunkStore never queries SimulationCore for pattern matching.
- **Pattern matching is explicit** — SimulationCore matches against a canonical registry. No inference, no heuristics, no emergent machine types.
- **Controllers are unified entities** — A MultiblockController represents the entire machine as one entity, not as a set of individual blocks.

---

# MultiblockController Lifecycle

A MultiblockController goes through four distinct states:

## 1. Pending Creation

The pattern detection system identifies a potential match. A PendingController is temporarily created to hold the detected pattern data while waiting for anchor resolution.

```cpp
struct PendingController {
    uint64_t id;           // Temp ID
    PatternMatch match;    // Detected block configuration
    uint32_t anchor;       // Resolved anchor position
    bool anchorInsideChunk; // For unload coordination
};
```

## 2. Active

The controller is fully formed and actively running. It participates in:

- Pattern detection (emits BlockChanged for its blocks)
- Controller registry (tracked by SimulationCore)
- Chunk unload coordination (anchorInsideChunk = true)
- State persistence (multiblock data serialized to MetaDB on destruction)

```cpp
struct MultiblockController {
    uint64_t id;           // Persistent ID
    PatternMatch match;    // Canonical pattern
    uint32_t anchor;       // Resolved anchor
    uint32_t chunk;        // Anchor's chunk
    bool active;           // True when running
    std::vector<Slot> slots;   // Input/output/shared
    SharedInventory inventory;
    EnergyStorage energy;
    bool destroyed;       // Graceful shutdown flag
};
```

## 3. Destroyed

The controller has been shut down but not yet persisted. During destruction:

- All slots are cleared
- Inventory contents are serialized to MetaDB
- Energy is drained
- AnchorInsideChunk determines ChunkStore behavior (see below)

## 4. Serialized

The controller's data has been persisted to MetaDB. The MultiblockController is removed from SimulationCore's registry. On world load, the controller is reconstructed from serialized data.

---

# Pattern Detection Mechanism

Pattern detection runs continuously in SimulationCore's main loop. It operates through a three-phase process:

## Phase 1: Subscription

SimulationCore subscribes to BlockChanged events from the MessageRouter. Each event carries:

- Block coordinates (x, y, z)
- Previous block id and meta
- New block id and meta
- Change source (client, generator, etc.)

```cpp
void onBlockChanged(uint32_t x, uint32_t y, uint32_t z);
```

## Phase 2: Pattern Matching

For each BlockChanged event, SimulationCore checks if the changed block belongs to any active PendingController's pattern. The match succeeds when:

1. The new block matches the expected pattern block
2. All previously matched blocks remain unchanged
3. The pattern is complete (all required blocks present)

```cpp
bool matchesPattern(const PatternMatch& pattern, uint32_t x, uint32_t y, uint32_t z);
```

## Phase 3: Anchor Resolution

When a pattern completes, SimulationCore determines the anchor — the position that defines the multiblock's spatial reference. Anchor resolution follows these rules:

1. Prefer the **first block** in the pattern (lowest x, then y, then z)
2. If multiple candidates exist, choose the one with the **highest solid block id** (prioritizes machines over decorative blocks)
3. If all candidates are air, fall back to the first block

```cpp
uint32_t resolveAnchor(const PatternMatch& match);
```

## Phase 4: Controller Creation

After anchor resolution, SimulationCore creates a MultiblockController:

1. Assign a new persistent ID
2. Record the anchor position and anchor's chunk
3. Register in the controller registry
4. Publish MultiblockCreated to the MessageRouter
5. Set mb_id for all blocks in the pattern (via RPC to ChunkStore)

```cpp
void createController(const PatternMatch& match, uint32_t anchor);
```

---

# Anchor Resolution

The anchor is the multiblock's spatial origin. It determines:

- Which chunk the multiblock is "in"
- Whether the multiblock is held during chunk unload
- Where the controller persists

## Anchor Selection Rules

```
Rule 1: First Block
  Select the block with lowest (x, y, z) coordinates.

Rule 2: Priority by Block ID
  If multiple blocks have the same coordinates (e.g., a decorative block
  on top of a machine block), select the one with the highest solid block id.

Rule 3: Air Fallback
  If all candidates are air, fall back to Rule 1.
```

## Anchor Inside vs. Outside Chunk

```
┌─────────────────────────────────────────────────────────┐
│  Anchor Inside Chunk                                    │
│  ──────────────────────────────────────────────────────  │
│  • Multiblock is fully contained in one chunk           │
│  • ChunkStore can serialize the controller              │
│  • ChunkStore releases the chunk on destroy              │
│  • Safe to unload without external dependencies          │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│  Anchor Outside Chunk                                   │
│  ──────────────────────────────────────────────────────  │
│  • Multiblock spans multiple chunks                      │
│  • Anchor chunk depends on another multiblock             │
│  • ChunkStore asks SimulationCore: "hold or release?"    │
│  • ChunkStore returns "hold" if anchor is outside        │
│  • Chunk stays in memory until all dependencies release  │
└─────────────────────────────────────────────────────────┘
```

---

# Block Ownership

## SimulationCore Owns Controllers

```
SimulationCore
├── MultiblockController[id=1]
│   ├── PatternMatch (canonical)
│   ├── Anchor (spatial origin)
│   ├── Slots (I/O, shared)
│   ├── Inventory (shared)
│   └── Energy (shared)
```

SimulationCore is the **sole authority** on:

- Creating new controllers
- Destroying controllers
- Modifying controller state
- Querying controller data

## ChunkStore Stores References Only

```
ChunkStore
├── Chunk[x,y,z]
│   ├── BlockData[block_x][block_y][block_z]
│   │   ├── block_id
│   │   ├── meta
│   │   └── mb_id  ← only reference
│   └── mb_ids (cached list for unload queries)
```

ChunkStore **never**:

- Knows what multiblocks are
- Queries SimulationCore for pattern matching
- Creates or destroys controllers
- Holds controller state

## Data Flow

```
┌─────────────────┐
│ Client places   │
│ block at (x,y,z)│
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Gateway routes  │
│ BlockChanged    │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ SimulationCore  │
│ • Matches pattern│
│ • Resolves anchor│
└────────┬────────┘
         │
         │ RPC: SetBlockMeta(mb_id)
         ▼
┌─────────────────┐
│ ChunkStore      │
│ • Updates meta  │
│ • Stores mb_id  │
└─────────────────┘
```

---

# Chunk Unload Coordination

When a chunk is marked for unload, ChunkStore follows this protocol:

## Step 1: Collect mb_ids

ChunkStore queries its cached mb_ids list and prepares a batch of queries.

## Step 2: Query SimulationCore

For each mb_id, ChunkStore sends an RPC:

```cpp
ChunkStore -> SimulationCore:
  "Can I unload chunk [x,y,z]? Anchor is insideChunk = <bool>"

SimulationCore -> ChunkStore:
  "release" | "hold"
```

## Step 3: Act on response

**release** — ChunkStore proceeds with unload:
- Serializes the multiblock to MetaDB
- Removes controller from registry
- Unloads chunk normally

**hold** — ChunkStore keeps chunk in memory:
- Skips unload for this chunk
- Retries on next tick
- Continues until all "hold" responses are resolved

## Why this matters

Without this coordination, a multiblock with an anchor outside its chunk would be lost when the chunk unloads. SimulationCore needs to know the anchor's location to safely persist the multiblock state.

---

# File Locations

## SimulationCore
- Pattern detection: `src/services/simulation_core/`
- MultiblockController: `src/services/simulation_core/src/multiblock_controller.h`
- Pattern matching: `src/services/simulation_core/src/pattern_detection.h`
- Anchor resolution: `src/services/simulation_core/src/anchor_resolution.h`

## ChunkStore
- Chunk data: `src/services/chunk_store/`
- Meta-layer storage: `src/services/chunk_store/src/chunk.h`
- Unload coordination: `src/services/chunk_store/src/chunk_store.h`

## Protocol
- BlockChanged message: `src/protocol/block_changed.fbs`
- MultiblockCreated message: `src/protocol/multiblock_created.fbs`
- SetBlockMeta RPC: `src/protocol/set_block_meta.fbs`

---

# Acceptance Criteria

## Scenario 1: Pattern Detection

Given a flat world with a 3×3×3 pattern of special blocks

When the last block is placed

Then SimulationCore detects the pattern within one tick

And SimulationCore resolves the anchor to the first block

And SimulationCore creates a MultiblockController

And ChunkStore updates all 27 blocks with the multiblock ID

And a MultiblockCreated message is published

## Scenario 2: Controller Creation

Given an active SimulationCore with a completed pattern

When the pattern is complete

Then a MultiblockController with a unique persistent ID is created

And the controller is registered in SimulationCore's registry

And the controller's anchor is set to the resolved position

And the controller's anchor chunk is recorded

And the controller's active flag is set to true

## Scenario 3: Chunk Unload Hold

Given a multiblock whose anchor is outside its primary chunk

When that primary chunk is marked for unload

Then ChunkStore queries SimulationCore for each multiblock in the chunk

And SimulationCore returns "hold" for the anchor multiblock

And ChunkStore keeps the chunk in memory

And ChunkStore retries the unload on the next tick

## Scenario 4: Chunk Unload Release

Given a multiblock whose anchor is inside its primary chunk

When the multiblock is destroyed

Then SimulationCore serializes the multiblock to MetaDB

And SimulationCore returns "release" to ChunkStore

And ChunkStore unloads the chunk normally

And the multiblock controller is removed from SimulationCore's registry

---
