## 1. Protocol & Schema (prerequisite — unblocks all serialization)

- [x] 1.1 Add `MultiblockState` table (`src/protocol/multiblock_state.fbs`: controller_id, dimension, anchor_x/y/z, pattern_id, packed_blocks, heat_stored, recipe_progress, recipe_ticks, recipe_id)
- [x] 1.2 `sim.multiblock.created` / `sim.multiblock.destroyed` schemas — `MultiblockCreatedEvent`/`MultiblockDestroyedEvent` already in `core.fbs`
- [x] 1.3 `HatchType` enum already in `core.fbs` (ITEM_INPUT..MUFFLER)
- [x] 1.4 `flatc` regenerated C++ stubs (`simcored_fbs` target OK)
- [x] 1.5 MessageRouter topics self-register on subscribe/publish — no `router.go` change needed

## 2. Generic Pattern Library (replaces matchElectrolyser hardcode)

- [x] 2.1 `MultiblockPattern` struct in `PatternLibrary.h` (pattern_id, name, controller_block_id, size, layers, hatches, controller_dx/dy/dz)
- [x] 2.2 `PatternRegistry` — `addPattern()` / `matchAll()` / `matchById()` via `BlockLookupFn` callback (ECS registry lookup, not ChunkStore RPC)
- [x] 2.3 EBF pattern (3×3×4): casing + coil layers + controller 1003
- [x] 2.4 Large Boiler pattern (3×3×4): firebox + controller 1005
- [x] 2.5 LCR pattern (3×3×3): casing + controller 1006
- [x] 2.6 `matchPattern()` via `PatternRegistry::matchAll()` — iterates ECS registry blocks
- [x] 2.7 `onBlockChanged()` calls `matchAll()` when controller block placed
- [x] 2.8 `ELECTROLYSER_PATTERN` + `matchElectrolyser()` kept for backward compat

## 3. Hatch Detection

- [x] 3.1 Hatch roles defined per-pattern as `HatchDef` (ITEM_IN/OUT, FLUID_IN/OUT, ENERGY, MUFFLER) in `PatternLibrary.h`
- [ ] 3.2 `findHatches(controller_pos, pattern)` active scan — deferred (design R2: hatch positions are pattern-relative, hardcoded for L2)
- [ ] 3.3 Hatch slots on `MultiblockController` — deferred with 3.2
- [ ] 3.4 Hatch slot mapping in recipe tick — systems read `slots_in` from MachineRegistry instead
- [ ] 3.5 `SideConfig` per-face hatch config — deferred to L3

## 4. Multiblock Tick Systems

- [x] 4.1 **EBFSystem** — coil tier (Kanhal 1800K / Nichrome 2700K / TungstenSteel 4500K), heat-gated recipe processing, EU consumption, outputs to container, BlockEntityUpdate publish
- [x] 4.2 **LargeBoilerSystem** — solid fuel (COAL/CHARCOAL) → HeatIntakeComponent, steam via PipeEnergyClient publishNodeUpdate, cooldown, overheat
- [x] 4.3 **LCRSystem** — recipe dispatch + energy gating + outputs (pattern 3)
- [x] 4.4 MachineSystem already skips `managed_externally` machines (Pass1 line 96, Pass2 line 171) — no double-processing

## 5. Dissociation

- [x] 5.1 `onBlockChanged(pos, AIR)` checks old `mb_id` on controller
- [x] 5.2 mb_id is ECS-only for L2 (never written to ChunkStore) — no `setBlockMeta` cleanup needed
- [x] 5.3 `MultiblockController` erased from `controllers_` map
- [ ] 5.4 Eject hatch contents — deferred (no item-entity spawn infra in simcore)
- [x] 5.5 Publish `sim.multiblock.destroyed` via `publishMultiblockDestroyed` (wired through `onMultiblockDestroyed`)
- [x] 5.6 Non-anchor block removal → `removeBlockFromController()` flow preserved

## 6. Persistence via EntityStateStore

- [x] 6.1 `MultiblockState` flatbuffer in `multiblock_state.fbs`
- [x] 6.2 `SimulationEngine::serializeMultiblock(mb_id)` + `onMultiblockSave` → `SaveEntityState(0, anchor, entity_type=4)`
- [x] 6.3 `SimulationEngine::deserializeMultiblock(mb_id, blob)` → restores heat/recipe on controller entity
- [ ] 6.4 Chunk-unload hook — no chunk-unload event exists in simcore; save-on-dissociation implemented instead
- [x] 6.5 Chunk-load: `onMultiblockCreated` → `LoadEntityState(0, x, y, z, 4)` → `deserializeMultiblock`
- [x] 6.6 Dissociation overwrites final state via save (no DeleteEntityState RPC exists)

## 7. Topic Wiring

- [x] 7.1 Topics `sim.multiblock.created` (mb_id, anchor, mb_type) / `sim.multiblock.destroyed` (mb_id) — dynamic registration
- [x] 7.2 SimulationCore publishes on match success (`publishMultiblockCreated`) and on dissociation (`publishMultiblockDestroyed`)
- [x] 7.3 Gateway subscribes + forwards as `GatewayMsg::kMultiblockEvent` (ctrl connection, `gateway.h:23`)
- [ ] 7.4 Client receives (NetClient logs) but no multiblock machine GUI — deferred (Non-Goal: client multiblock visuals)
