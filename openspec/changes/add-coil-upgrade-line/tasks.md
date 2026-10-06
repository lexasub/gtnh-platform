## 1. Correct the constants that point at nothing

- [ ] 1.1 Fix `KANHAL_COIL_BLOCK_ID` — it spells "kanhal"; the item is
      `kanthal_ingot`. Decide whether the item name follows the constant or the
      constant follows the item, then make them agree
- [ ] 1.2 Add the missing items for `BLAST_CASING_BLOCK_ID` (`1110:111:5`),
      `NICHROME_COIL_BLOCK_ID` (`1110:111:6`) and
      `TUNGSTENSTEEL_COIL_BLOCK_ID` (`1110:111:7`) — currently constants with no
      item to place. This is the gp-831t defect
- [ ] 1.3 Verify every `PatternLibrary.h` constant resolves to an `items.csv` row,
      and add a test that asserts it so the gap cannot reopen

## 2. Casings

- [ ] 2.1 Add nine casing tiers LV→UV in `1110:111` (deferred plan proposes
      40-49; confirm those slots are still free and unclaimed)
- [ ] 2.2 Craft recipes: casing tier N from casing tier N-1 plus plates, matching
      GTNH's escalation
- [ ] 2.3 Confirm the EBF and HBF patterns both accept casings, or document why
      only one does

## 3. The coil ladder

- [ ] 3.1 Add the ten coil items. Three already have constants (kanthal,
      nichrome, tungstensteel); add: hss_g, hss_s, naquadah, trinium, tpv,
      infinity, eternal, awakened_draconium
- [ ] 3.2 Add the Electrum Flux coil (GTNH has a quest for it; we have no
      analogue) — decide whether to add the coil or skip the quest
- [ ] 3.3 Allocate ids avoiding: live slots 0-4, 10-22, the `PatternLibrary.h`
      range 5-8, and the deferred plan's 40-81
- [ ] 3.4 Recipes: each coil from its alloy + copper + the tier below. Verify
      with `python3 tools/validate_items.py` after every batch

## 4. Tiered glass

- [ ] 4.1 Extract GTNH's glass chain from the modpack repo — nine tiers with
      their smelting machine, into `doc/research/gtnh-glass-tiers.md`. GTNH's
      quests confirm the tiers exist (`Better Glass Upgrade, EV Glass`, `IV
      Glass`, `LuV Glass`, `ZPM Glass`, `UV Glass`, `UHV Glass`, `UEV Glass`,
      `UIV Glass`, `UMV Glass`, all in `MultiblockGoals`) but the producing
      machine per tier must come from the tree, not from guesswork
- [ ] 4.2 Add `ev_glass`, `luv_glass`, `zpm_glass`, `uv_glass`, `uhv_glass`,
      `uev_glass`, `uiv_glass`, `umv_glass` to `items.csv` in a dedicated pool.
      Not `1110:111` — that pool is multiblock parts and is nearly full
- [ ] 4.3 Give `vacuum_freezer` (`1110:010:28`) and `implosion_compressor`
      (`1110:010:27`) machine variants — both exist as craftable items with no
      recipe class, so no recipe can target them today
- [ ] 4.4 Glass recipes: tier N+1 from tier N in the machine GTNH specifies.
      Each MUST be gated behind the coil that raises the heat ceiling
- [ ] 4.5 Glass quests in a `glass_tiers` section, one per tier, each stating
      what the tier buys (it is not "stronger glass", it is a heat rating)

## 5. Exotic hatches

- [ ] 5.1 Extend `HatchType` (`PatternLibrary.h:63-70`) with
      `DATA_ACCESS`, `VOID_BUS_INPUT`, `VOID_BUS_OUTPUT`, `FILTERED_BEAMLINE`
- [ ] 5.2 Register the hatch item ids and extend `hatchBlockIdToType` — an
      unhandled id returns `NONE`, so a hatch added to `items.csv` without a
      switch case occupies a slot and connects nothing, silently
- [ ] 5.3 Items: `data_access_hatch`, `void_bus_input`, `void_bus_output`,
      `filtered_beamline_output_hatch`
- [ ] 5.4 Craft recipes for each
- [ ] 5.5 Hatch quests in `multiblocks_junior`, explaining what each exotic type
      is for — a void bus is not "a better bus", it is a machine part you place
      to destroy surplus output
- [ ] 5.6 New test: every registered hatch id maps to a non-NONE `HatchType`

## 6. Quests (coils)

- [ ] 6.1 One quest per coil stage in a new `coils` section
- [ ] 6.2 Each quest description MUST state what the upgrade buys — heat
      ceiling, throughput, which alloy tier it unlocks — per
      `refactor-quests-to-explain`
- [ ] 6.3 Chain prerequisites: coil N+1 requires coil N, so the ladder is a path
      and not a list
- [ ] 6.4 Requirements in `quest_requirements.json` with packed ids

## 7. Verification

- [ ] 7.1 `ctest -R content_reachability` — every coil, glass tier and exotic
      hatch has a producer and no newly unreachable item
- [ ] 7.2 `ctest` — EBF/HBF multiblock tests still pass after the constant
      re-pointing, including the legacy 1003 alias path
- [ ] 7.3 `ctest -R quest_graph` — the coil and glass chains are valid DAGs, no
      cycles
- [ ] 7.4 New test: every `PatternLibrary.h` block-id constant resolves in the
      registry
- [ ] 7.5 New test: every registered hatch id maps to a non-NONE `HatchType`
- [ ] 7.6 `python3 tools/validate_items.py` after every id batch