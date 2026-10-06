## 1. Research (blocks everything else)

- [ ] 1.1 Extract the exact GTNH recipe tree from the modpack repo
      (`config/betterquesting/DefaultQuests/Quests/Tier1LV-*`,
      `Tier2MV-*`, `Tier3HV-*`, `WorkingwithOil-*`) into
      `doc/research/gtnh-chemistry-tree.md` — every intermediate, its inputs,
      and its gating quest. Source of truth is the BetterQuesting JSON, not
      wiki pages.
- [ ] 1.2 Map each GTNH ore-dict name onto our registry name. GTNH uses
      `dustSalt`-style names (`circuitAdvanced`, `plateSteel`) that do not match
      `advanced_circuit_hv`, `steel_plate`. Record every mismatch and its chosen
      target; a wrong mapping silently produces an unreachable quest.
- [ ] 1.3 Decide the fluid-vs-item split per chemical. Our model has fluids in
      `fluids.csv` (volume) AND item rows in `items.csv` (inventory), bridged
      only by `fluid_buckets.csv`. Each GTNH chemical needs an explicit ruling.

## 2. Machine classes

- [ ] 2.1 Add `fluid_solidifier` to `machines.yaml` with `solidifier_lv` and
      tier variants; recipes in `recipes/fluid_solidifier.yaml`
- [ ] 2.2 Add `sifting_machine` to `machines.yaml`; recipes in
      `recipes/sifting_machine.yaml`
- [ ] 2.3 Give `distillation_tower` (`1110:010:36`) a real `block_id` variant in
      `machines.yaml` — today it has none, so `findRecipesForMachine` can never
      resolve a recipe to it
- [ ] 2.4 Register the three classes in `RecipeManager.cpp` where machine classes
      are enumerated, and confirm `MachineSystem` starts them (it skips
      `managed_externally` machines — `SimulationEngine.cpp:251-256`)
- [ ] 2.5 Craft recipes for all three blocks in `crafting_table.yaml`

## 3. Registry

- [ ] 3.1 Allocate IDs for new chemistry items from free, unclaimed slots in
      `1111:11` (58 is oil; 59-71 taken by the previous batch; deferred plan
      claims 8-57). Verify with `python3 tools/validate_items.py`
- [ ] 3.2 Allocate IDs for `fluid_solidifier_lv/mv/hv` and
      `sifting_machine_lv/mv/hv` in `1110:010` (free from 65)
- [ ] 3.3 Add a `fluids.csv` row per new fluid (color, density, gaseous,
      temperature). `Registry::validateReferences` (`Registry.cpp:206`) fails the
      entire catalog load if a `fluids.csv` item_id does not resolve in
      `items.csv` — the two files MUST agree
- [ ] 3.4 Add `fluid_buckets.csv` rows for bucket-obtainable chemistry fluids

## 4. Recipes

- [ ] 4.1 Write the basics chain: `sulfur_ore → sulfur → sulfur_dioxide →
      sulfuric_acid`
- [ ] 4.2 Salts chain: `sodium → sodium_hydroxide → sodium_persulfate`
- [ ] 4.3 Halogens chain: `chlorine → hydrochloric_acid → chloroform`
- [ ] 4.4 Organic chain: `methane → acetylene → ethaone → ethanol`, and
      `acetylene → benzene → toluene → styrene → polystyrene`
- [ ] 4.5 Plastics chain: `ethaone → vinyl_chloride → pvc → plastic_sheet`,
      gated behind `fluid_solidifier`
- [ ] 4.6 Solvents chain: `methanol`, `ethanol`, `glycerol`, `acetone`,
      `acetaldehyde`, `acetic_acid`
- [ ] 4.7 Silicon chain: `silicon_dust → silicon_tetrachloride →
      trichlorosilane → silane → dimethyldichlorosilane`
- [ ] 4.8 Fuels chain: `naphtha → refinery_gas → butane/propane → lpg`, and the
      sulfuric fuels
- [ ] 4.9 Every recipe input MUST itself have a producer. `content_reachability_test`
      fails the build on any newly unreachable registry item, and `oil` /
      `hydrogen` are currently baseline debt with no producer — a chemistry
      recipe consuming them re-creates the hole.

## 5. Quest line

- [ ] 5.1 Add `chemistry` section rows to `quests.csv` across the sub-sections
      from the proposal
- [ ] 5.2 Add requirements to `quest_requirements.json` — one entry per quest,
      `kind: craft`/`obtain`, packed ids via `ItemId::pack`
- [ ] 5.3 Wire `quest_graph.json` prerequisites so each intermediate is gated
      behind its own producer (GTNH ordering, not just "all available")
- [ ] 5.4 Write quest descriptions in explaining style per
      `refactor-quests-to-explain` (each quest says WHY, not just what)

## 6. Verification

- [ ] 6.1 `python3 tools/validate_items.py` — no duplicate packed ids
- [ ] 6.2 `ctest -R content_reachability` — no newly unreachable item outside
      `reachability_baseline.inc`
- [ ] 6.3 `ctest -R recipe_item_resolution` — every recipe input/output resolves
- [ ] 6.4 `ctest` full run — no regression in `quest_graph_test`,
      `quest_manager_test`, `recipe_manager_matching_test`
- [ ] 6.5 `python3 tools/find_items_without_recipe.py` — no `[needed]` items
      without a producer