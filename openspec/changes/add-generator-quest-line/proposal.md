# Change: Quest line for power generation

## Why

We just added seven generators — `combustion_generator`, `diesel_generator_lv/mv/hv`,
`gas_turbine_lv/mv/hv` (`items.csv:1110:100:18-24`) — with craft recipes and
**zero quests**. GTNH's `HowtoGeneratePower` line is 136 quests and treats
generation as a progression axis, not a block list: fuel → refining →
efficiency → turbine → scaling. `Diesel Power` → `Even Better Diesel` →
`Incomplete Combustion` → `Gas Turbine` → `XL Turbo Gas Turbine` is a path where
each step is only worth taking because of the one before it.

Ours currently has nine power quests total (`lv_power`, `mv_power`, `hv_power` —
cables, transformers, battery buffers) and nothing about actually making power.

## What Changes

- A `power_generation` quest section with sub-sections: `gen_basics` (steam vs
  EU), `gen_fuels` (coal → oil → diesel → refined), `gen_combustion`,
  `gen_turbines`, `gen_scaling`.
- Fuel-chain items the quests need: `diesel`, `biodiesel`, `bio_ethanol`,
  `nitro_diesel`, `refined_gas`, `gasoline`, `lpg`.
- `steam_turbine` tier variants (LV/MV/HV) — one exists at `1110:010:44`, the
  quests need the tiers.
- `solar_panel` tiers (LV/MV/HV) from the deferred plan, with the
  compressed-solar ladder as quest stages only (no compressed-solar items yet —
  see Non-Goals).
- Turbine housing and coil-tier gating as quest explanations tying into
  `add-coil-upgrade-line`.
- Every generator block gets a quest explaining its trade-off, not just its
  existence.

## Impact

- Affected specs: `power-generation` (new), `questbook-quest-data`,
  `recipe-fluid-io`
- Affected code:
  - `src/content/data/registry/items.csv` (fuels in `1111:11`, panels and
    turbine tiers in `1110:100` / `1110:010`)
  - `src/content/data/registry/machines.yaml` (generator variants per tier)
  - `src/content/data/recipes/generator.yaml` — note a `class: generator` recipe
    **cannot declare item outputs**: `RecipeManager.cpp:812-818` skips the whole
    outputs block for `generator`/`boiler`. Generator blocks are therefore
    crafted in `crafting_table.yaml`, and their fuel recipes declare
    `energy_out` instead
  - `src/content/data/quests/*`

## Non-Goals

- No nuclear: no uranium/thorium/plutonium/naquadah fuel cycles, no reactor
  multiblocks. That is a 60-quest GTNH line and its own backlog entry.
- No magic generators (Essentia, Kinetic, thaumaturgic). Backlog.
- No Solar/Belldust advanced blocks — quest stages reference compressed solar
  as an explanation only, not as obtainable items.
- No rocket engines / Space Race. Backlog.
- No wind/water power. Backlog.