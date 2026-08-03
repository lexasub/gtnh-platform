# Change: Multiblocks L3 — Hatches, Item IO, Block-Break Guard, Client Visuals

## Why
`implement-multiblocks-l2` ships pattern matching, tick systems, and persistence. Hatch detection was partially deferred: the `findHatches()` scan and `MultiblockController.hatches` slot containers exist in code, but hatch slots are not wired into recipe ticks, per-face side config is not connected, hatch items are silently lost on dissociation and chunk unload, and the client has no multiblock status window. This change finishes the hatch story and corrects the eject semantics: no item-entity spawn — on block break, contents return to the player's inventory, and the block refuses to break if contents do not fit.

## What Changes
- **Hatch coordinate convention fixed**: hatch positions are **controller-relative** (`world_pos = controller_pos + HatchDef offset`). `findHatches()` already implements this; the L2 design doc was updated to match.
- Hatch block IDs use the hierarchical items.csv format; until `items.csv`/`machines.yaml` is updated (planned), the code uses a placeholder ID plus a TODO.
- Patterns declare `ITEM_IN`/`ITEM_OUT` hatches for **EBF and LCR only**; the Large Boiler keeps FLUID hatches (fuel still read from the controller container).
- Recipe ticks read inputs/outputs from hatch slot ranges (`getInputSlotRange`/`getOutputSlotRange`) instead of hardcoded `MachineRegistry` offsets.
- `SideConfig` per-face hatch configuration, wrenchable, connected to `HatchSlot.side_config`.
- **Block-break guard replaces eject**: on dissociation, hatch/container contents go to the breaking player's inventory; if they do not fit, the block is not broken.
- Hatch/inventory contents persisted in `MultiblockState` serialization.
- Client multiblock GUI: machine window for multiblock controllers (heat, recipe progress, hatches).

## Impact
- Affected specs: multiblocks-l3 (new)
- Affected code:
  - `src/services/simulation_core/ECS/PatternLibrary.h/.cpp` — hatch resolution, hatch block IDs, ITEM_IN/OUT pattern declarations
  - `src/services/simulation_core/ECS/components/MultiblockController.h`, `components/HatchSlot.h` — hatch slots, side config
  - `src/services/simulation_core/ECS/SimulationEngine.cpp` — hatch slot assignment, block-break guard, serialization
  - `src/services/simulation_core/ECS/Systems/EBFSystem.cpp`, `LargeBoilerSystem.cpp`, `LCRSystem.cpp` — hatch-based IO
  - `src/services/game_client/` — multiblock GUI
