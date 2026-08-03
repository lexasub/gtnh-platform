# Change: Multiblocks L3 — Hatches, Eject, Client Visuals

## Why
`implement-multiblocks-l2` ships pattern matching, tick systems, and persistence, but hatch detection was deferred (hatch positions are pattern-relative, hardcoded for L2). This change tracks the L3 follow-up so the work is not lost: active hatch scanning, hatch slot mapping in recipe ticks, per-face side config, eject of hatch contents, and client-side multiblock visuals.

## What Changes
- Active `findHatches()` scan resolving pattern-relative hatch positions to world positions.
- Hatch slots on `MultiblockController`; systems read hatch inventory via slots from MachineRegistry.
- `SideConfig` per-face hatch configuration (wrenchable).
- Eject hatch contents on dissociation (requires item-entity spawn infra in simcore).
- Client multiblock GUI (was a Non-Goal of L2).

## Impact
- Affected specs: multiblocks-l3 (new)
- Affected code:
  - `src/services/simulation_core/ECS/PatternLibrary.h/.cpp` — hatch resolution
  - `src/services/simulation_core/ECS/components/MultiblockController.h` — hatch slots
  - `src/services/simulation_core/ECS/Systems/EBFSystem.cpp`, `LargeBoilerSystem.cpp`, `LCRSystem.cpp` — hatch-based IO
  - `src/services/game_client/` — multiblock GUI
