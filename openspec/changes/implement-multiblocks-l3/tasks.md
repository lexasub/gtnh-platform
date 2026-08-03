## 1. Hatch Detection

- [ ] 1.1 `findHatches(controller_pos, pattern)` active scan — resolve pattern-relative hatch positions to world positions (`PatternLibrary.h`)
- [ ] 1.2 Hatch slots on `MultiblockController` — inventory containers per hatch role
- [ ] 1.3 Hatch slot mapping in recipe tick — systems read `slots_in`/`slots_out` from hatch containers instead of hardcoded offsets
- [ ] 1.4 `SideConfig` per-face hatch config — wrenchable per-face INPUT/OUTPUT/FLUID_IN/FLUID_OUT/ENERGY

## 2. Dissociation

- [ ] 2.1 Eject hatch contents — on dissociation, spawn hatch items as world entities (requires item-entity spawn infra in simcore)

## 3. Client

- [ ] 3.1 Client multiblock GUI — machine window for multiblock controllers (heat, recipe progress, hatches) on `kMultiblockEvent`/`BlockEntityUpdate`
