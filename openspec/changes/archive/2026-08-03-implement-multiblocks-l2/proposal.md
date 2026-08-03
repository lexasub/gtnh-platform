# Change: Implement Multiblocks L2

## Why

L1 multiblock prototype exists (electrolyser pattern matching via `matchElectrolyser()`, `MultiblockController` ECS entity, `registerController()`). But L2 gameplay is missing — no generic pattern matching, no EBF/Large Boiler/LCR tick logic, no dissociation, no hatch detection, no multiblock persistence, no `sim.multiblock.*` topics. The codebase has a **stub** SpatialIndex (`src/services/spatial_index/main.cpp:1` — `int main() { return 0; }`) and hardcoded `ELECTROLYSER_PATTERN` (`src/services/simulation_core/ECS/SimulationEngine.cpp:20`).

For detailed design reference, see the archived EPIC: `doc/archive/EPICS/7-multiblocks-l2/7-multiblocks-l2.md`.

## What Changes

### Core Architecture Decision: Defer SpatialIndex
SpatialIndex is a stub (empty service). Instead of implementing a full R-tree/Octree service, L2 uses **direct ChunkStore pattern matching**: `onBlockChanged` checks each pattern offset via `ChunkStore.getBlock()` — O(blocks_in_pattern) per check. For EBF (3×3×4 = 28 blocks excluding controller) this is acceptable for MVP. SpatialIndex deferred to L3.

### Changes by Module

**SpatialIndex Service** (`src/services/spatial_index/`):
- No code change. Service remains stub. L2 explicitly defers it.

**SimulationCore — Pattern Library** (`src/services/simulation_core/ECS/`):
- Replace hardcoded `ELECTROLYSER_PATTERN` + `matchElectrolyser()` with generic `MultiblockPattern` struct and `PatternRegistry`.
- Define patterns for EBF (3×3×4), Large Boiler (multi-size: 1×1×1 through 3×3×4), LCR (3×3×3).
- `matchPattern(pattern, anchor_pos)` — iterates ChunkStore offsets, returns `PatternMatchResult`.
- Trigger: `onBlockChanged(pos)` checks ChunkStore `mb_id` and runs pattern match for registered patterns.

**SimulationCore — EBF Tick** (`src/services/simulation_core/ECS/Systems/`):
- New `EBFSystem` (or integrated into `MachineSystem` via `managed_externally` flag).
- Heating coil block ID → heat tier mapping (Kanhal 1800K, Nichrome 2700K, TungstenSteel 4500K).
- Recipe heat requirement check (via `RecipeManager`).
- Input/output/energy hatch slots in `InventoryContainer`.

**SimulationCore — Large Boiler Tick** (`src/services/simulation_core/ECS/Systems/`):
- New `LargeBoilerSystem`.
- Firebox inventory slot for solid fuel (coal/charcoal).
- Water input hatch → steam conversion → PipeNetwork fluid output.
- Overheat detection when water runs out.
- Multi-size support (1×1×1 → 3×3×4).

**SimulationCore — LCR Tick** (`src/services/simulation_core/ECS/Systems/`):
- New `LCRSystem`.
- Fluid + solid input recipes via `RecipeManager` `findRecipe()`.
- Fluid hatch management (input fluid → consume, output fluid → produce).
- Byproduct handling.

**SimulationCore — Dissociation** (`src/services/simulation_core/ECS/SimulationEngine.cpp`):
- Check anchor break in `onBlockChanged(pos, AIR)`.
- Clear `mb_id` from all pattern blocks via ChunkStore.
- Eject hatch contents.
- Publish `sim.multiblock.destroyed`.

**SimulationCore — Hatch Detection**:
- Classify block sides as input/output/energy/fluid hatches.
- Use `SideConfig` component for per-face configuration.
- Expose hatch slots in `InventoryContainer` for recipe tick.

**MessageRouter** (`src/services/message_router/`):
- Register `sim.multiblock.created` and `sim.multiblock.destroyed` topics.
- Subscribe SimulationCore publisher, Gateway consumer.

**EntityStateStore** (`src/services/entity_state_store/`):
- Define serialization format for `MultiblockState` (controller id, anchor, blocks list, hatches, progress data).
- Save/load via existing `SetEntityStateReq`/`GetEntityStateReq` RPC.

**Protocol** (`src/protocol/`):
- Add `MultiblockState` table to `entity_state_store.fbs` for persistence (or reuse existing blob storage).
- Add `sim.multiblock.created`/`destroyed` message schemas to `core.fbs`.
- **BREAKING**: `WorldBlockEntityUpdate` may gain multiblock-specific fields (structure_valid, heat_level).

## Impact

- **Affected specs**: multiblocks-l2 (new delta)
- **Affected code**:
  - `src/services/simulation_core/ECS/SimulationEngine.h/.cpp` — generic pattern matching replaces `matchElectrolyser`
  - `src/services/simulation_core/ECS/SimulationEngine.h` — remove `ELECTROLYSER_PATTERN` extern
  - `src/services/simulation_core/ECS/Systems/EBFSystem.h/.cpp` — new
  - `src/services/simulation_core/ECS/Systems/LargeBoilerSystem.h/.cpp` — new
  - `src/services/simulation_core/ECS/Systems/LCRSystem.h/.cpp` — new
  - `src/services/simulation_core/ECS/Systems/MachineSystem.cpp` — hook multiblock tick
  - `src/services/simulation_core/ECS/Systems/HeatTransferSystem.cpp` — may need multiblock-aware heat transfer
  - `src/services/simulation_core/ECS/components/MultiblockController.h` — may extend
  - `src/services/simulation_core/main.cpp` — register new systems
  - `src/services/simulation_core/Network/SimCoreMessageHandler.cpp` — subscribe `sim.multiblock.*`
  - `src/services/simulation_core/Network/clients/EntityStateStoreClient.h/.cpp` — multiblock save/load
  - `src/services/entity_state_store/main.cpp` — handle MultiblockState
  - `src/protocol/entity_state_store.fbs` — add MultiblockState table
  - `src/protocol/core.fbs` — add multiblock event schemas
  - `src/services/message_router/` — topic registration

## Assumptions & Deferrals

- **SpatialIndex deferred**: L2 uses ChunkStore direct pattern checks (O(n)). SpatialIndex (R-tree/Octree) stays stub.
- **No client visuals for L2**: No multiblock highlight, bounding box, or special rendering. Player sees machine GUI only.
- **Existing `BoilerSystem` untouched**: The current single-block boiler (`BoilerSystem.cpp`) handles steam_solid_boiler / steam_heat_boiler. LargeBoilerSystem is separate.
- **L2 reuses `managed_externally` flag**: EBF/Boiler/LCR use the existing `MachineComponent.managed_externally = true` flow for recipe routing.
- **Electrolyser L1 kept**: Existing `ELECTROLYSER_PATTERN` + `matchElectrolyser()` kept for backward compat. New patterns use generic `PatternRegistry`.
