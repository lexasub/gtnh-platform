## 1. Model (`RecipeTypes.h`)
- [ ] 1.1 Add `FluidIOItem { uint16_t fluid_id; uint32_t amount_mb; }` next to InputItem/OutputItem
- [ ] 1.2 Add `fluid_inputs` / `fluid_outputs` vectors to `Recipe`
- [ ] 1.3 Add `hasFluidInputs()/hasFluidOutputs()` helpers + `fluidAmountPerOperation()` sum

## 2. Parsing (`RecipeManager.cpp`)
- [ ] 2.1 Parse `fluid_inputs` / `fluid_outputs` YAML sequences (reuse item-name resolution; `amount` required, positive)
- [ ] 2.2 Store parsed fluid IO on `Recipe` (mirror InputItem/OutputItem parse pattern)

## 3. Validation
- [ ] 3.1 Extend `validateResourceRequirements` (or sibling) to check every fluid IO id: exists in items.csv AND has fluids.csv row; amount > 0
- [ ] 3.2 Keep behavior: one bad fluid name fails the whole load, no partial recipes

## 4. Execution (`MachineSystem.cpp`)
- [ ] 4.1 Reserve fluid inputs via `CraftReservationClient` before craft start (4.1.4) — do not consume item inputs until fully accepted
- [ ] 4.2 On completion, credit fluid outputs into machine `FluidStorage` output tank
- [ ] 4.3 Expose output fluids for drain through FLUID source port (ResourceBufferState publisher keeps observing component state)

## 5. Tests
- [ ] 5.1 Parsing: fluid_inputs/outputs by name and packed id; zero amount rejected; unknown fluid rejected
- [ ] 5.2 Reservation: craft waits on short tank, starts after full acceptance, item inputs consumed once
- [ ] 5.3 Completion: outputs credited to FluidStorage, observable via publisher
- [ ] 5.4 Regression: existing 734 simcored tests still pass; bucket/steam paths untouched

## 6. Sample recipes
- [ ] 6.1 Convert one existing chemical_reactor reaction to fluid schema to prove the path (e.g. water_split stays item-based; add electrolyser fluid prototype only if param chain exists)