# Architecture Decision Record Risk Analysis
## SimulationCore + Gateway - ECS/Gateway Design

**Analysis Date**: 2026-06-01  
**Scope**: ADR Sections 6 (Gateway) and 8 (ECS Design)

---

## Section 6: Gateway

### Decision 6.1: io_uring on Linux
- **Status in ADR**: "В разработке" (In development) — underspecified
- **Risk**: LOW
  - **Platform**: Linux-only. macOS/Windows explicitly out of scope. io_uring is the correct Linux-native async I/O backend for Asio.
  - **Must decide before**: io_uring vs epoll fallback for older Linux kernels (&lt;5.1). Asio auto-selects the best available backend.
- **Recommendation**: Use Asio with io_uring (default on Linux ≥5.1). No abstraction needed — project targets Linux only.

### Decision 6.2: No chunk cache
- **Status in ADR**: Explicit decision — Gateway is TCP mux/demux only
- **Risk**: LOW. Gateway is intentionally stateless. Chunk retransmission is handled by ChunkStore on next request. 192 KB per chunk is small — even at 100 chunks/s it's ~19 MB/s, well within TCP capacity.
- **Recommendation**: Keep as-is. Add caching later if traffic profiling shows it's a bottleneck.

### Decision 6.3: Routing (PlayerAction→SimulationCore, InventorySync→MetaDB, ChunkRequest→ChunkStore)
- **Status in ADR**: Listed but underspecified
- **Risk**: CRITICAL
  - **Failure scenario**: Router doesn't know `ChunkStore` address. No topic definition for `ChunkRequest`. If `ChunkStore` fails, what happens to `ChunkRequest`? Does it fail open (continue to other clients) or close the connection?
  - **Must decide before**: Router topology (tree? mesh?), failure mode, topic naming convention
- **Recommendation**: Define explicit Router topology (e.g., Hub-and-Spoke with Router as Hub, Services as Spokes). Define `ChunkRequest` topic: `#chunk/store` or `store.chunks`. Define error handling: 50% of requests should retry, 50% drop connection

---

## Section 8: ECS Design

### Decision 8.1: Granular components (Position, Inventory, MachineState, EnergyStorage)
- **Status in ADR**: Component types listed
- **Risk**: MEDIUM
  - **Failure scenario**: MachineStateComponent doesn't define `recipeId`. What if multiblock requires a `multiblockType` component? What about `FacingComponent`, `PoweredComponent`, `FluidLevelComponent`? Missing components force ad-hoc `component_id` hacks.
  - **Must decide before**: Full component taxonomy for multiblocks, pipes, entities
- **Recommendation**: Define full component set:
  - `MultiblockTypeComponent` { mb_type, controller_id, anchor }
  - `FacingComponent` { direction }
  - `PoweredComponent` { powered, power_source }
  - `FluidLevelComponent` { fluid_id, level, capacity }

### Decision 8.2: Coordinate → Entity lookup (std::unordered_map<uint64_t, entt::entity>)
- **Status in ADR**: O(1) lookup specified
- **Risk**: CRITICAL
  - **Failure scenario**: `pack(x,y,z)` collides across chunks. If two chunks have the same (x,y,z) offset but different chunk coords, `unpack` produces identical key. Lookup returns wrong entity.
  - **Must decide before**: Coordinate space definition (absolute world coords? chunk-local coords?), packing/unpacking algorithm
- **Recommendation**: Use **absolute world coordinates** for keys. Define `pack(x,y,z) = ((uint64_t)x << 40) | ((uint64_t)y << 22) | (uint64_t)z`. Unpack: `x = key >> 40`, `y = (key >> 22) & 0x3FF`, `z = key & 0x3FF`. No collisions possible with unique world coords.

### Decision 8.2 (coord system): Implicit world-coordinate assumption
- **Status in ADR**: NOT EXPLICIT
- **Risk**: CRITICAL
  - **Failure scenario**: SimulationCore uses chunk-local coords (0-31), but ECS uses absolute coords. When client sends `PlayerAction(x=100, y=64, z=200)`, ECS lookup fails because no entity exists at absolute coord (100,64,200).
  - **Must decide before**: Absolute vs. chunk-local coordinate systems
- **Recommendation**: ECS **must** use absolute world coordinates. ChunkStore uses chunk-local (0-31), but ECS uses absolute. Define conversion in ECS layer, not Router.

---

## Missing Decisions

### 6.4: TCP connection lifecycle
- Not covered: What happens when a client disconnects? Does Gateway send a `ClientDisconnected` message to Router? How long does a connection stay in `active` state after EOF?
- **Recommendation**: Gateway must emit `ClientDisconnected(player_id)` on EOF. Define 30-second grace period before dropping connection

### 6.5: Message ordering guarantees
- Not covered: Does Router guarantee FIFO? What if a `PlayerAction` arrives after `InventorySync` for the same tick?
- **Recommendation**: Define ordering contract: Router must deliver messages in arrival order. No reordering. If out-of-order detected, drop oldest and mark client as "stale"

### 6.6: Multiblock controller lifecycle
- Not covered: How does a MultiblockController get destroyed? What happens when a player breaks the anchor block?
- **Recommendation**: Define `MultiblockDestroyed(controller_id)` event emitted by ECS. ChunkStore must receive `SetBlockMeta(mb_id=0)` for all blocks in the destroyed multiblock

### 8.3: Entity cleanup / GC
- Not covered: When does an entity get destroyed? Is there a `LifetimeComponent`?
- **Recommendation**: Entities should have `LifetimeComponent { last_seen, ticks_idle }`. ECS must run `ticks_idle` check every tick, removing entities > `max_idle` ticks

### 8.4: Multiblock pattern matching
- Not covered: How does ECS detect 3×3×3 patterns? What happens if a multiblock is partially formed (2×2×2)?
- **Recommendation**: Implement pattern matcher: scan all `MultiblockTypeComponent` entities, check neighbors within 3-block radius. If 7+ blocks match 3×3×3 pattern → emit `MultiblockCreated`

---

## Implementation Ordering Dependencies

```
Phase 1 (Data Layer):
  - Implement IChunkStore with LMDB (chunk serialization, SetBlock/GetBlock)
  - Implement ChunkStore routing: ChunkRequest → ChunkStore

Phase 2 (Simulation Layer):
  - Implement ECS with PositionComponent, InventoryComponent
  - Implement Coordinate→Entity lookup using ABSOLUTE world coords
  - Implement pattern matcher (3×3×3 scan)
  - Implement MultiblockController lifecycle

Phase 3 (Storage Layer):
  - Implement MetaDB with SQLite (players table)
  - Implement InventorySync routing

Phase 4 (Networking Layer):
  - Implement Router topology (Hub-and-Spoke)
  - Implement io_uring on Linux (epoll fallback for other platforms)
  - Implement ChunkRequest routing
  - Define ChunkSnapshot message format

Phase 5 (Client Layer):
  - Implement GameClient: bgfx, FPS camera, DDA raycast
  - Implement PlayerAction routing
  - Implement ChunkStore getPartialChunk (for retransmission)
```

---

## Summary

**Most Critical Risks**:
1. io_uring Linux-only (6.1) — breaks on Windows/macOS
2. Coordinate system ambiguity (8.2) — breaks multiblock formation
3. Chunk cache absence (6.2) — wastes bandwidth on retransmission

**Must Decide Before Coding**:
1. Absolute vs. chunk-local coords → **ABSOLUTE**
2. io_uring fallback → **epoll on Linux, Select elsewhere**
3. Chunk retransmission → **ChunkStore must support getPartialChunk**
4. Router topology → **Hub-and-Spoke**
5. Connection lifecycle → **EOF → emit disconnect → 30s grace → drop**
