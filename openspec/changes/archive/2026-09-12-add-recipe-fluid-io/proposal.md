# Change: Add fluid inputs/outputs to recipe schema (fluid-inputs-outputs)

## Why

Chemistry recipes are currently item-bound: fluids appear as bucket items
(`water_bucket`, `oil_bucket`) or as per-tick `resource_requirements` (steam).
GT-style chemistry needs recipes that consume and produce *fluid masses* per
operation (e.g. electrolyser: 2000 mB water -> 2000 mB H2 + 1000 mB O2;
distillation: oil -> fractions). The transport layer is already prepared:
typed FLUID ports, drain/consume transactions, `FluidStorage` component, and
machine-side reservation via `CraftReservationClient` (openspec 4.1.4).

## What Changes

- Recipe YAML schema gains two optional fields:
  `fluid_inputs: [{ fluid: <name>, amount: <mB> }]` and
  `fluid_outputs: [{ fluid: <name>, amount: <mB> }]` — per-operation fluid
  masses (mB), resolved through the canonical items.csv id domain.
- `Recipe` model gains `fluid_inputs` / `fluid_outputs` vectors
  (`src/libs/recipe_manager_lib/RecipeTypes.h`), parsed and validated in
  `RecipeManager.cpp` (same name resolution as items: `ItemRegistry::nameToId`,
  then confirmed present in `fluids.csv`).
- `MachineSystem` reserves `fluid_inputs` through the existing
  `CraftReservationClient` FLUID path *before* the craft starts and consumes
  item inputs (4.1.4); a completed recipe credits `fluid_outputs` into the
  machine's `FluidStorage` output tank, which then drains via the FLUID source
  port.
- Existing `resource_requirements` (per-tick steam) and bucket-item recipes
  stay unchanged — this is additive, not a migration.
- Fills the item/fluid gap exposed by the previous chemistry work: GT-style
  reactions (electrolysis, oil cracking/distillation, polymerisation) become
  expressible as pure fluid recipes.

## Impact

- Affected specs: `recipe-id-format` (name resolution rules reused for fluid
  names), new `recipe-fluid-io` capability.
- Affected code: `src/libs/recipe_manager_lib/RecipeTypes.h`,
  `src/libs/recipe_manager_lib/RecipeManager.cpp`,
  `src/services/simulation_core/ECS/Systems/MachineSystem.cpp`,
  `src/services/simulation_core/ECS/components/FluidStorage.h`,
  test fixtures (`simcored_test`, recipe parse tests).
- **Not BREAKING**: existing recipe YAMLs without the new fields parse
  unchanged; existing bucket/steam paths untouched.
- Out of scope (next change): filling out electrolyser/distillation recipe
  chains, bucket <-> fluid converter machine, multi-liquid tanks.