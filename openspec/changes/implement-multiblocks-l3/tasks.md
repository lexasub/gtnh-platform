## 1. Hatch Detection

- [x] 1.1 `findHatches(controller_pos, pattern)` active scan — resolve controller-relative hatch positions to world positions (`PatternLibrary.h`) *(already in code: `PatternLibrary.cpp` `findHatches()`; fixed: no longer drops hatches at negative world coords)*
- [x] 1.2 Hatch slots on `MultiblockController` — inventory containers per hatch role *(already in code: `MultiblockController.hatches`, `HatchSlot.h`, `assignHatchSlots()`)*
- [x] 1.2a Hatch block IDs in hierarchical items.csv format (`"X:XX:X"`); placeholder ID + TODO until `items.csv`/`machines.yaml` is updated — `HATCH_BLOCK_*` in `PatternLibrary.h` use `ItemId::pack("1110:10:N")` with a TODO (distinct from the legacy 1001-1006 controller/structural ids)
- [x] 1.3 Hatch slot mapping in recipe tick — systems read `slots_in`/`slots_out` from hatch slot ranges (`getInputSlotRange`/`getOutputSlotRange`) instead of `MachineRegistry` offsets; EBF/LCR declare `ITEM_IN`/`ITEM_OUT` hatches, boiler keeps FLUID hatches (fuel via controller container) — `PatternLibrary.cpp` patterns + `EBFSystem.cpp` (rewritten from a stub), `LCRSystem.cpp`, `LargeBoilerSystem.cpp`
- [x] 1.4 `SideConfig` per-face hatch config — wrenchable per-face INPUT/OUTPUT/FLUID_IN/FLUID_OUT/ENERGY, connected to `HatchSlot.side_config` — `WrenchHandler` cycles `HatchSlot.side_config` for hatches

## 2. Dissociation

- [x] 2.1 Block-break guard — on block/dissociation, hatch & container contents return to the breaking player's inventory; if they do not fit, the block SHALL NOT break (no item-entity spawn) — `SetBlockCASHandler` guard + `SimulationEngine::findControllerAt`/`collectControllerContents`/`destroyController` (replaces the broken eject stub)
- [x] 2.2 Persist hatch/container contents in `MultiblockState` serialization (so nothing is lost on chunk unload) — `multiblock_state.fbs` `slots` field + `serializeMultiblock`/`deserializeMultiblock`

## 3. Client

- [x] 3.1 Client multiblock GUI — machine window for multiblock controllers (heat, recipe progress, hatches) on `kMultiblockEvent`/`BlockEntityUpdate` — `MachineWindow` parses/renders `hatches`; `RouterEventPublisher` maps simcore→Protocol `HatchType` correctly; `GameClient` runtime-registers controllers so the window opens; controllers get 4+4 slot grids mapping onto hatch slots
