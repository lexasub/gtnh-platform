# ADR Analysis — RecipeManager & SimulationCore

## 3. RecipeManager

### Decision: Separate service (NOT in SimulationCore) — OK
- **Status in ADR**: RecipeManager is a separate C++ service. SimulationCore NOT subscribed to its topics initially.
- **Risk**: LOW. Clear separation of concerns. RecipeManager can restart independently. Future hot-reload without stopping SimulationCore.

### Decision: RPC via Router (request-response with correlationId) — MEDIUM
- **Status in ADR**: SimulationCore → Router → RecipeManager.CheckRecipe and back.
- **Risk**: MEDIUM. Router is pub/sub, not request-response. correlationId-based matching adds complexity. What if RecipeManager response arrives after SimulationCore timeout?
- **Must decide before**: Timeout values for RPC calls. What happens on timeout — retry? skip tick? cache stale?
- **Recommendation**: Define `timeout_ms` per RPC. On timeout, SimulationCore uses cached recipe (if available) or skips tick with warning.

### Decision: L1 caching in SimulationCore — MEDIUM
- **Status in ADR**: SimulationCore caches CheckRecipe results per machine. Invalidated on inventory change.
- **Risk**: MEDIUM. Cache invalidation mechanism not defined. How does SimulationCore know inventory changed? Via BlockChanged event from ChunkStore — but what if inventory changes in EntityStateStore without block change?
- **Must decide before**: Invalidation trigger protocol. Does ChunkStore publish InventoryChanged event? Does EntityStateStore?
- **Recommendation**: Two invalidation paths: (1) BlockChanged event from ChunkStore (block ID changes), (2) InventoryChanged event from EntityStateStore (slot changes without block change). Cache TTL as fallback.

### Missing: Recipe persistence format
- **Current**: Recipes hardcoded in RecipeManager::Init() stub
- **Risk**: HIGH. No persistence means all recipes lost on restart. No hot-reload, no validation.
- **Recommendation**: Define JSON schema for recipe files in `data/recipes/`. Load at startup.

## 5. SimulationCore Machine Tick

### Decision: needsTick polling — HIGH
- **Status in ADR**: SimulationCore iterates all machines every tick (20 Hz). Idle machines skip recipe lookup via needsTick flag.
- **Risk**: HIGH. Polling all machines every tick doesn't scale. With 10000 machines, loop overhead alone costs CPU even when most are idle. needsTick only skips recipe lookup, not the iteration itself.
- **Must decide before**: Machine count target. Event-driven scheduling (only tick machines with pending work) vs polling.
- **Recommendation**: Use event-driven scheduling: machines register for tick via needsTick flag. On inventory change / energy change → set needsTick. On tick complete → clear needsTick. Only iterate machines with needsTick=true.

### Decision: State streaming — two levels — MEDIUM
- **Status in ADR**: Level 1 (interest radius, 1-2 Hz) sends isRunning flag. Level 2 (GUI open, 20 Hz) sends full state.
- **Risk**: MEDIUM. WatchEntity/UnwatchEntity mechanism not implemented. Client disconnect without UnwatchEntity leaves stale watched entries.
- **Must decide before**: Client disconnect handling — does SimulationCore auto-unwatch on player disconnect?
- **Recommendation**: Store watched entities per player in ECS component PlayerWatchedMachines. On player disconnect event → clear all watches for that player.

### Decision: Multiblocks NOT in ECS — HIGH
- **Status in ADR**: Multiblocks stored in separate MultiblockRegistry. ECS only for single-block machines and entities.
- **Risk**: HIGH. Dual-path serialization — ECS entities serialize one way, MultiblockRegistry another. EntityStateStore expects uniform component-based access.
- **Must decide before**: Serialization format for multiblocks. How does EntityStateStore save/restore multiblock state?
- **Recommendation**: Reconsider. Store MultiblockController as a tagged ECS component. Single entity per multiblock, not 36 block entities. Simplifies serialization, querying, and chunk crossing.

### Decision: Chunk unload coordination — MEDIUM
- **Status in ADR**: ChunkStore → SimCore: ChunkUnloadRequest. SimCore → ChunkStore: released_mb_ids[] / hold_mb_ids[].
- **Risk**: MEDIUM. Not implemented yet. Race condition possible if machine state changes between unload request and response.
- **Must decide before**: What locks are held during unload coordination? Can a block change arrive during unload?
- **Recommendation**: Sequenced unload: (1) ChunkStore marks chunk pending_unload, (2) stops accepting writes, (3) sends ChunkUnloadRequest, (4) SimCore processes, (5) ChunkStore unloads. Block changes during unload are queued.

## Cross-Cutting

### ECS 1:1 block:entity ratio
- Inefficient for multiblocks. 36-block furnace = 36 entities + separate registry entry.
- Recommend: single entity per multiblock, block positions stored as component data.

### Missing: MachineStateComponent serialization format
- No format defined for EntityStateStore blob. MachineStateComponent + InventoryComponent + EnergyStorageComponent need binary layout.
- Recommend: FlatBuffer schema for TileEntity state blob.
