## 1. Research

- [ ] 1.1 Extract GTNH `HowtoGeneratePower` (136 quests) into
      `doc/research/gtnh-generation-line.md` — for each generator: fuel,
      efficiency path, what it unlocks, and the gating quest
- [ ] 1.2 Extract the fuel chain from `WorkingwithOil` (45 quests): crude →
      naphtha → refinery gas → fuels. Our `oil` has **no producer** (recorded
      reachability debt) and the whole chain hangs off it — decide whether this
      change fixes oil or explicitly defers it

## 2. Fuel items

- [ ] 2.1 Add `diesel`, `biodiesel`, `bio_ethanol`, `nitro_diesel`,
      `refined_gas`, `gasoline`, `lpg` to `items.csv` (`1111:11` free slots,
      avoiding the deferred plan's claims)
- [ ] 2.2 Add matching `fluids.csv` rows — `validateReferences` fails the whole
      catalog if they disagree
- [ ] 2.3 Add `fluid_buckets.csv` rows for the bucket-carried fuels
- [ ] 2.4 Fuel-chain recipes: biodiesel from seed oil, nitro diesel from
      biodiesel + nitro, refinery gas from cracking

## 3. Generator machines

- [ ] 3.1 Add `steam_turbine_lv/mv/hv` variants to `machines.yaml` under the
      existing `steam_turbine` class
- [ ] 3.2 Add per-tier variants for `diesel_generator` and `gas_turbine` so each
      tier is a distinct block with its own energy_output
- [ ] 3.3 Add `solar_panel_lv/mv/hv` items and machine variants from the deferred
      plan (`1110:100:12-14`)
- [ ] 3.4 Generator FUEL recipes go in `generator.yaml` with `energy_out` and NO
      `outputs:` block — a `class: generator` recipe can never yield an item
      (`RecipeManager.cpp:812-818`). Generator BLOCKS are crafted in
      `crafting_table.yaml`, matching the existing creative-generator precedent

## 4. Quests

- [ ] 4.1 `power_generation` section with sub-sections `gen_basics`,
      `gen_fuels`, `gen_combustion`, `gen_turbines`, `gen_scaling`
- [ ] 4.2 Requirements in `quest_requirements.json`, prerequisites in
      `quest_graph.json`
- [ ] 4.3 Descriptions in explaining style per `refactor-quests-to-explain`:
      each generator quest states the trade-off (EU/t vs fuel consumption, why
      the next tier is worth it)
- [ ] 4.4 Coil gating quests link to the `add-coil-upgrade-line` ladder

## 5. Verification

- [ ] 5.1 `ctest -R content_reachability` — no newly unreachable fuel
- [ ] 5.2 `ctest -R recipe_manager_matching` — generator class recipes parse and
      the fuel recipes declare no item outputs
- [ ] 5.3 `ctest` — no regression in `machine_system_test`, `quest_graph_test`