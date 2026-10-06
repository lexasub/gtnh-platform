## ADDED Requirements

### Requirement: Generation quests state a trade-off

Every generator quest SHALL describe what the generator costs and what it buys,
rather than only naming the block.

#### Scenario: Generator quest explains its trade-off

- **GIVEN** a quest for `diesel_generator_mv`
- **THEN** its description SHALL state the energy output, the fuel it consumes,
  and why the MV tier is worth building over the LV tier
- **AND** the description SHALL NOT be a bare item name or a one-line "craft X"

#### Scenario: Fuel quests precede the generator that burns them

- **GIVEN** a generator quest whose block consumes `diesel`
- **THEN** the quest producing `diesel` SHALL be a prerequisite
- **AND** with `diesel` not craftable the generator quest SHALL be LOCKED

#### Scenario: Compressed solar referenced as explanation, not as an item

- **GIVEN** a quest describing the compressed-solar ladder
- **THEN** the description MAY reference solar multipliers as concept text
- **AND** the quest SHALL NOT declare a requirement on a compressed-solar item,
  because no such item exists in the registry

### Requirement: Generator blocks are craftable items

Every generator block SHALL be obtainable as an inventory item, and generator
fuel recipes SHALL NOT declare item outputs.

#### Scenario: Generator block obtained by crafting

- **GIVEN** `diesel_generator_mv` is a generator-class block
- **THEN** a recipe in `crafting_table.yaml` SHALL produce it as an item
- **AND** it SHALL be reachable, so `content_reachability_test` passes

#### Scenario: Generator fuel recipe produces energy, not items

- **GIVEN** a recipe with `class: generator` that consumes fuel
- **THEN** it SHALL NOT declare an `outputs:` block naming an item
- **AND** it SHALL declare `energy_out` instead
- **AND** authoring an item output SHALL be a review failure, because
      `RecipeManager.cpp:812-818` skips the outputs block entirely for
      `generator`/`boiler` classes — the recipe would parse clean and craft
      nothing

### Requirement: Generation line is gated by tier progression

The `power_generation` quest line SHALL order its sub-sections so that a player
cannot reach a turbine or a scaled generator before the fuel chain that feeds it.

#### Scenario: Sub-section ordering

- **GIVEN** the `power_generation` section
- **THEN** its sub-sections SHALL be `gen_basics`, `gen_fuels`,
      `gen_combustion`, `gen_turbines`, `gen_scaling`
- **AND** the order SHALL reflect the dependency chain, not alphabetical order

#### Scenario: Turbine tier requires its fuel

- **GIVEN** `gas_turbine_hv` consumes refined gas
- **THEN** the quest producing refined gas SHALL be a prerequisite of the
      `gas_turbine_hv` quest