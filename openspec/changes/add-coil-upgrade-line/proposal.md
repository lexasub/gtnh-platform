# Change: Blast furnace ladder — coils, casings, glass tiers, exotic hatches

## Why

GTNH's blast-furnace progression is a ten-step coil ladder — Kanthal,
Nichrome, HSS-G, HSS-S, Naquadah, Trinium, TPV, Infinity, Eternal, Awakened
Draconium — where each coil raises the heat ceiling and unlocks the next alloy
tier. It is the single longest-lived progression axis in the game after MV.

We have four constants in `PatternLibrary.h:19-22` (`BLAST_CASING`,
`KANHAL_COIL`, `NICHROME_COIL`, `TUNGSTENSTEEL_COIL`) and **none of them exist
in `items.csv`** — slots `1110:111:5-8` are empty. So the blast furnace cannot
currently be built from real blocks; the pattern matcher resolves to ids nothing
can place. GTNH itself has twelve coil quests in `MultiblockGoals`, including
Electrum Flux which we have no analogue for.

Two more areas sit on the same machinery and are bundled here rather than left
dangling in the content backlog:

- **Tiered glass.** GTNH has nine glass tiers (LuV, ZPM, UV, UHV, UEV, UIV, UIV,
  UMV, plus EV). They are the same blast-furnace ladder seen from the other
  side: each tier is smelted at the heat ceiling the previous coil unlocks. We
  have `vacuum_freezer` (`1110:010:28`) and `implosion_compressor` as items with
  craft recipes, but no tiered-glass items and no machine class for either.
- **Exotic hatches.** `HatchType` (`PatternLibrary.h:63-70`) has six values. GTNH
  adds Data Access, Void Buses, Filtered Beamline Output, and the Radium /
  Neutronium hatches. All of them are hatch variants on the same multiblock part
  slots the ladder already occupies, and all of them share the same
  find-hatches path.

## What Changes

- Ten coil items added to `items.csv` in the `1110:111` pool, covering the
  existing three constants plus the seven missing stages.
- **BREAKING** (data): `PatternLibrary.h` coil constants stop pointing at ids
  that no item occupies. `KANHAL_COIL_BLOCK_ID` currently spells "kanhal"; the
  item is `kanthal_ingot`. Constants are re-pointed at real ids.
- Nine casing tiers (LV through UV) in `1110:111`, matching GTNH's
  `1110:111:40-49` deferred plan.
- Nine tiered-glass items (`ev_glass`, `luv_glass`, `zpm_glass`, `uv_glass`,
  `uhv_glass`, `uev_glass`, `uiv_glass`, `umv_glass`) plus a machine class for
  the freezer/compressor pair that produces them.
- Five exotic hatch types: `data_access_hatch`, `void_bus_input`,
  `void_bus_output`, `filtered_beamline_output_hatch`, and the radium/neutronium
  pair. **BREAKING** (code): `HatchType` gains values.
- Coil recipes: each coil crafted from its alloy plus copper, escalating in cost
  and tier requirement.
- Quest stages in a new `coils` section (one per coil), a `glass_tiers` section
  (one per tier), and hatch quests in `multiblocks_junior` — each explaining what
  the upgrade buys, not just naming the item.
- `machines.yaml` gains `vacuum_freezer` and `implosion_compressor` variants;
  coils, casings, glass and hatches remain multiblock parts, not machines.

## Impact

- Affected specs: `blast-furnaces` (modified), `multiblocks-l2` (modified —
  HatchType), `questbook-quest-data`
- Affected code:
  - `src/engine/sim/PatternLibrary.h` (coil constants, HatchType enum,
    `hatchBlockIdToType` switch)
  - `src/content/data/registry/items.csv` (`1110:111` slots 5-9, 40-49; new
    glass pool; new hatch ids)
  - `src/content/data/registry/machines.yaml` (freezer, implosion compressor)
  - `src/content/data/recipes/crafting_table.yaml`, `recipes/blast_furnace.yaml`
  - `src/content/data/quests/*`
- Closes bead **gp-831t** ("5 real items under the migrated 1110 pool are not
  classified in any subgroup") — the 3 missing coils plus `blast_casing` and one
  more.
- Risk — constants: `PatternLibrary.h` coil constants are `constexpr`, consumed
  by tests and by both client and server (`GameClient.cpp:255-259`,
  `simcore/main.cpp:234-243`). Re-pointing them changes what the EBF/HBF patterns
  match. A legacy alias path (`LEGACY_EBF_CONTROLLER_BLOCK_ID = 1003`) must keep
  working.
- Risk — HatchType: extending the enum changes `findHatches` for every multiblock.
  A new hatch id that falls through `hatchBlockIdToType` returns `NONE`, so a
  mis-added hatch silently occupies a slot without connecting anything. Needs a
  test asserting every registered hatch id maps to a non-NONE type.
- Removes glass tiers and exotic hatches from the `add-gtnh-content-backlog`
  pending list.

## Non-Goals

- No electromagnetic (Superconductor) coils — a separate mechanic.
- No Plasma/MK-coil variants.
- No multiblock boosters (parallel / time / struct / meta upgrades). Those are
  a distinct mechanism — per-multiblock upgrade slots — not more parts, and they
  stay in the backlog.
- No conduit or pipe tiers — `pipes-cables-transport` is its own capability.
- No coil behaviour changes: this is content plus the constant re-pointing, not
  a rebalance.
- No Coil telemetry / superconductivity thresholds; coils remain parts.