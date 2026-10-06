# Change: GTNH chemistry progression as first-class content

## Why

The registry now carries 13 chemistry items (`items.csv:1111:11:59-71`) with 13
recipes, but there is no quest, no section, and no progression gating any of
them. In GTNH chemistry is not a row in a table — it is a 40-recipe tree with
intermediates that the player must discover in order, and it is the primary
reason to keep playing past LV. Right now a player can craft methanol on turn
one, with nothing in the quest book telling them it exists or why.

Two of the three machines the tree needs do not exist at all as recipe classes:
`fluid_solidifier` and `sifting_machine` are absent from `items.csv`, and
`distillation_tower` exists at `1110:010:36` but has no variant in
`machines.yaml`, so no recipe can ever target it.

## What Changes

- **BREAKING** (data, not schema): three new recipe classes in `machines.yaml`
  — `fluid_solidifier`, `sifting_machine`, `distillation_tower` — each with its
  own `recipes/*.yaml`. `distillation_tower` gains its first real block_id.
- New chemistry items beyond the current 13: the organic intermediates
  (`benzene`, `toluene`, `styrene`, `ethylene_oxide`, `acetone`, `acetaldehyde`,
  `acetic_acid`, `vinyl_chloride`, `glycerol`, `methanol`, `ethanol`,
  `ethanone`, `butane`, `propane`, `lpg`, `naphtha`, `refinery_gas`,
  `sulfuric_gas`) and the silicon-chlorine line (`silicon_tetrachloride`,
  `trichlorosilane`, `silane`, `dimethyldichlorosilane`).
- New chemistry reagents the tree needs as inputs: `sulfur_dioxide`,
  `hydrochloric_acid`, `sodium_hydroxide`, `sodium_persulfate`, `acetylene`,
  `nitrogen_dioxide`, `sulfuric_naphtha`, `light_fuel`, `heavy_fuel`.
- A quest line `chemistry` with sub-sections mirroring the GTNH progression:
  `chem_basics` (sulfur → sulfuric acid), `chem_salts` (sodium, NaOH,
  persulfate), `chem_halogens` (chlorine → HCl → chloroform), `chem_organic`
  (benzene → toluene → styrene → polystyrene), `chem_plastics` (vinyl chloride
  → PVC → plastic sheets), `chem_solvents` (ethanol, methanol, glycerol,
  acetone), `chem_silicon` (SiCl4 → trichlorosilane → silane), `chem_fuels`
  (naphtha → refinery gas → LPG → sulfuric fuels).
- `fluid_solidifier` and `sifting_machine` as craftable blocks with MV/HV tiers.

## Impact

- Affected specs: `chemistry-progression` (new), `recipe-fluid-io`,
  `questbook-quest-data`, `recipe-id-format`
- Affected code:
  - `src/content/data/registry/items.csv` (new IDs in `1111:11`, `1110:010`,
    `1110:001`)
  - `src/content/data/registry/machines.yaml` (3 new classes)
  - `src/content/data/recipes/` (3 new files + chemistry expansion in
    `chemical_reactor.yaml`)
  - `src/content/data/registry/fluids.csv` (every new fluid)
  - `src/content/data/registry/fluid_buckets.csv`
  - `src/content/data/quests/quests.csv`, `quest_requirements.json`,
    `quest_graph.json`
- Risk: the recipe-fluid-io capability has a known bug (`refactor-fluid-port-accounting`,
  0/30 tasks). Chemistry recipes are the heaviest fluid-port consumer in the
  game, so this change will surface that bug rather than hide it.

## Non-Goals

- No combustion chemistry, no explosives, no nuclear reprocessing.
- No Thaumaturgy / Blood Magic / Mystical Agriculture chemistry (separate backlog).
- No petrochemistry past what the fuel chain needs (cracking is
  `fluid_solidifier`/`chemical_reactor` class work, listed in tasks).