## Context

L2 multiblocks (EBF, Large Boiler, LCR) add gameplay depth to the existing L1 multiblock prototype (electrolyser). The change spans 4 services (simulation_core, entity_state_store, message_router, gateway) and the protocol layer. Key constraint: SpatialIndex service is a stub and will not be implemented for L2.

Implementation agent reads: `doc/archive/EPICS/7-multiblocks-l2/7-multiblocks-l2.md` for detailed architecture reference, pattern diagrams, and tick flow.

## Goals / Non-Goals

### Goals
- Replace hardcoded `ELECTROLYSER_PATTERN` + `matchElectrolyser()` with generic `MultiblockPattern` + `PatternRegistry`
- Implement EBF tick (heating coils, heat requirement, muffler)
- Implement Large Boiler tick (firebox, water→steam, overheat, multi-size)
- Implement LCR tick (fluid chemistry, catalysts, byproducts)
- Implement hatch detection (ITEM_IN/OUT, FLUID_IN/OUT, ENERGY, MUFFLER)
- Implement dissociation (anchor break → full cleanup)
- Persist multiblock state via EntityStateStore
- Wire `sim.multiblock.created`/`destroyed` topics

### Non-Goals
- SpatialIndex R-tree/Octree implementation (deferred to L3)
- Client multiblock visuals (highlight, bounding box, special rendering)
- Steam turbine integration (separate scope)
- Transformers (separate scope, `TransformerSystem` already exists)
- Item/fluid transport via PipeNetwork for multiblock hatches (PipeNetwork exists but integration is deferred)
- Changing existing `BoilerSystem` (single-block boiler stays as-is)

## Decisions

### Decision 1: Defer SpatialIndex — Use ChunkStore Direct Pattern Checks
**Decision**: L2 does NOT implement SpatialIndex. Pattern matching iterates `ChunkStore.getBlock()` for each offset in the pattern.
- EBF (3×3×4) = 28 getBlock calls per pattern check
- Large Boiler (3×3×4) = 28 calls
- LCR (3×3×3) = 27 calls
- **Why**: Acceptable for MVP. ChunkStore is local (same process or fast RPC). Actual pattern checks only trigger on `onBlockChanged` for controller blocks, not every tick.
- **Deferred to L3**: Full SpatialIndex with R-tree + Octree in `src/services/spatial_index/`.

### Decision 2: Generic Pattern Struct — No Inheritance
**Decision**: `MultiblockPattern` is a plain struct with data fields (layers, offsets, hatch defs), not a polymorphic class.

```cpp
struct PatternLayer {
    std::vector<std::vector<uint16_t>> rows; // block_id grid, 0 = wildcard
};

struct HatchDef {
    int32_t dx, dy, dz; // relative to anchor
    HatchType type;     // ITEM_IN, ITEM_OUT, FLUID_IN, FLUID_OUT, ENERGY, MUFFER
};

struct MultiblockPattern {
    uint32_t id;
    std::string name;
    PatternLayer layers[4]; // max 4 layers for L2
    std::vector<HatchDef> hatches;
    int32_t controller_dx, controller_dy, controller_dz;
};
```

- **Why**: Simpler than virtual `IMultiblockTick` interface. L2 only needs 3 patterns. Tick dispatch uses a switch on `pattern_id` rather than virtual dispatch.
- **Alternatives considered**: Virtual `IMultiblockSystem` interface per multiblock type (overkill for 3 patterns).

### Decision 3: Separate ECS Systems Per Multiblock Type
**Decision**: Three separate `ISystem` implementations: `EBFSystem`, `LargeBoilerSystem`, `LCRSystem`.

- **Why**: Each tick logic is distinct (coils vs firebox vs chemistry). Separating avoids a giant switch statement.
- **File locations**:
  - `src/services/simulation_core/ECS/Systems/EBFSystem.h/.cpp`
  - `src/services/simulation_core/ECS/Systems/LargeBoilerSystem.h/.cpp`
  - `src/services/simulation_core/ECS/Systems/LCRSystem.h/.cpp`
- Registered in `main.cpp` via `simulationEngine->registerSystem()`.

### Decision 4: EBF Heating Coil Tier Mapping
**Decision**: Block ID → heat tier mapping, hardcoded in `EBFSystem`:

| Coil Block ID | Tier | Max Heat |
|---------------|------|----------|
| `KANHAL_COIL` | 1 | 1800K |
| `NICHROME_COIL` | 2 | 2700K |
| `TUNGSTENSTEEL_COIL` | 3 | 4500K |

- Detected by scanning the heating coil layer of the pattern during construction (not every tick).
- Stored on `MultiblockController` as `heat_tier` field (extension of struct).

### Decision 5: Large Boiler Multi-Size via Separate Pattern Entries
**Decision**: Define separate `MultiblockPattern` entries for each boiler size (1×1×1, 2×2×2, 3×3×4), not a generic parametric size.
- **Why**: Each size has different block requirements, hatch positions, and controller offset. Parametric sizing adds complexity without gameplay benefit for L2.
- Heat capacity scales linearly with pattern volume.

### Decision 6: MultiblockState FlatBuffer Serialization
**Decision**: Define `MultiblockState` as a FlatBuffers table for EntityStateStore persistence:

```fbs
table MultiblockState {
    controller_id: uint64;
    dimension: int32;
    anchor_x: uint32;
    anchor_y: uint32;
    anchor_z: uint32;
    machine_id: uint16;
    pattern_id: uint32;
    blocks: [uint32];       // packed xyz positions
    heat_stored: int32;     // HeatIntakeComponent.heat_stored
    recipe_progress: int32; // RecipeProgress.current
    recipe_ticks: int32;    // total ticks for current recipe
}
```

- Stored as opaque blob via existing `SetEntityStateReq`/`GetEntityStateReq` RPC.
- Key = `(dimension, anchor_x, anchor_y, anchor_z, entity_type=MULTIBLOCK)`.
- **Why**: Reuses existing EntityStateStore infrastructure. No new RPCs needed.

### Decision 7: sim.multiblock.* Topic Schema
**Decision**: Add message schemas to `core.fbs`:

```fbs
table MultiblockCreated {
    mb_id: uint64;
    controller_x: int32;
    controller_y: int32;
    controller_z: int32;
    pattern_type: uint16;
}

table MultiblockDestroyed {
    mb_id: uint64;
}
```

- Published on `sim.multiblock.created` / `sim.multiblock.destroyed` topics.
- SimulationCore = publisher. Gateway = subscriber (forwards to client).

## Risks / Trade-offs

- **R1: O(n) pattern matching without SpatialIndex** → For MVP scale (<1000 multiblocks) this is fine. If world size grows, L3 SpatialIndex becomes mandatory.
- **R2: Hatch detection hardcodes positions** → Hatch positions are pattern-relative (defined in `MultiblockPattern.hatches`). Player must build hatches at exact positions. No dynamic hatch recognition. Accepted for L2.
- **R3: EntityStateStore load on chunk load** → Each chunk might need 1+N lookups (1 for multiblocks, N for tile entities). Acceptable for L2; batch loading can be optimized in L3.
- **R4: Three separate systems vs one generic system** → Code duplication between systems (energy hatch read, recipe dispatch). Mitigation: factor shared multiblock helpers into `MultiblockUtils.h`.

## Migration Plan

1. **Add new code alongside existing** — `ELECTROLYSER_PATTERN` and `matchElectrolyser()` stay untouched.
2. **Implement PatternLibrary + PatternRegistry** first (unblocks everything).
3. **Implement multiblock tick systems** one at a time (EBF → Boiler → LCR).
4. **Wire dissociation** (modifies existing `onBlockChanged` flow).
5. **Add persistence** last (requires all state fields settled).
6. **Add topic wiring** after systems are stable.

## Open Questions

- Should hatch detection use `SideConfig` component (per-face config) or be purely pattern-relative? Current intent: pattern-relative for L2, `SideConfig` in L3.
- Should EBF/Large Boiler/LCR share a common `MultiblockTickSystem` base class? Current: no, separate ISystem implementations. Can refactor if duplication grows.
- What entity_type value to use for MultiblockState in EntityStateStore? Suggestion: `entity_type = 4` (MULTIBLOCK_STATE), distinct from existing 1/2 values.
