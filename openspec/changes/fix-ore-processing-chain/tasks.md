## Phase 0: Audit & Strategy
- [ ] 0.1 Verify `ItemId::pack()` computed IDs against RecipeManager YAML parser behavior (`src/libs/recipe_manager_lib/RecipeManager.cpp:464-468`)
- [ ] 0.2 Confirm resolution path: `item: iron_ore` → `stoi()` fails → `resolveItemName("iron_ore")` → `ItemRegistry::nameToId()` → packed uint16_t
- [ ] 0.3 Compute correct `ItemId::pack()` values for existing items.csv entries (iron_ore=32768, iron_ingot=24577, etc.)
- [ ] 0.4 Compute reserved ID ranges for new crushed/plate items, ensure no collisions

## Phase 1: Item Registry (`data/registry/items.csv` — single source of truth)
- [ ] 1.1 Add crushed ore items with unique hierarchical IDs: crushed_iron, crushed_gold, crushed_copper, crushed_tin, crushed_lead, crushed_silver, crushed_zinc
- [ ] 1.2 Add metal plate items: iron_plate, gold_plate, copper_plate, tin_plate, lead_plate, silver_plate, zinc_plate
- [ ] 1.3 Verify all new entries have non-colliding `ItemId::pack()` values by running the pack function
- [ ] 1.4 Verify existing entries (iron_ore, gold_ore, ingots, bronze_plate) have correct IDs and string names that recipes will use

## Phase 2: Macerator Recipes (`data/recipes/macerator.yaml`) — string names only
- [ ] 2.1 Replace ALL numeric `item:` values with string names (e.g., `item: iron_ore`, `item: crushed_iron`)
- [ ] 2.2 Each ore type outputs its own crushed variant — not a shared dust ID
- [ ] 2.3 Add missing ore type recipes: lead, silver, zinc
- [ ] 2.4 Ensure HEAT/STEAM/ROTATION energy variants exist for core ore types
- [ ] 2.5 Verify `findRecipeByInputs()` matches correctly with string-resolved IDs

## Phase 3: Furnace Recipes (`data/recipes/furnace.yaml`) — string names only
- [ ] 3.1 Replace ALL numeric `item:` with string names
- [ ] 3.2 Add crushed_X→X_ingot recipes for all 7 metal types using string names
- [ ] 3.3 Add missing ore→ingot recipes for copper, tin, lead, silver, zinc using string names

## Phase 4: Compressor Recipes (`data/recipes/compressor.yaml`) — string names only
- [ ] 4.1 Replace ALL numeric `item:` with string names
- [ ] 4.2 Fix `compress_iron` output from `iron_ingot` to `iron_plate`
- [ ] 4.3 Add X_ingot→X_plate recipes for all 7 metal types using string names
- [ ] 4.4 Verify compress_bronze (bronze_ingot→bronze_plate) still correct

## Phase 5: Integration Verification
- [ ] 5.1 Audit: grep all recipe YAML files for `item: \d+` patterns — zero hits expected (no numeric IDs)
- [ ] 5.2 Verify every string item name in recipes resolves via `ItemRegistry::nameToId()`: name exists in items.csv and returns nonzero uint16_t
- [ ] 5.3 Check `MachineRegistry` block_id parsing: machines.yaml strings like "1110:00:1" → uint16_t (note: current parser returns prefix only, not packed ID — confirm block class resolution still works)
- [ ] 5.4 Build project: `cd cmake-build-debug && ninja -j5` (or release)
- [ ] 5.5 Run tests: `ctest --output-on-failure -j$(nproc)`
- [ ] 5.6 Manual chain test: place macerator + furnace + compressor with item pipes, verify ore→crushed→ingot→plate completes with correct item IDs
