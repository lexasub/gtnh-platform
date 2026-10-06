## ADDED Requirements

### Requirement: Chemistry machines are addressable recipe classes

The system SHALL register `fluid_solidifier`, `sifting_machine` and
`distillation_tower` as machine classes whose blocks can be resolved by
`findRecipesForMachine`, and each SHALL have a dedicated recipes file.

#### Scenario: Distillation tower resolves to its class

- **GIVEN** `distillation_tower` has an `items.csv` row at `1110:010:36`
- **WHEN** a recipe declares `class: distillation_tower`
- **THEN** placing the block SHALL let `findRecipesForMachine` return that
  recipe
- **AND** the block SHALL be present in `classByBlockId_` (not dropped by the
  `mv.block_id > 0` guard at `RecipeManager.cpp:381`)

#### Scenario: Each chemistry machine has its own class

- **GIVEN** a `class: fluid_solidifier` recipe and a `class: sifting_machine`
  recipe
- **THEN** placing the solidifier SHALL NOT offer the sifting recipe
- **AND** sharing one class between the two SHALL be a spec violation, because
  `findRecipesForMachine` resolves by class and a shared class hands every block
  every recipe of the pool

#### Scenario: Machine declares no fluid fields

- **GIVEN** a chemistry machine variant
- **THEN** the variant SHALL declare no fluid fields, because `MachineVariant`
  (`MachineRegistry.h:16-31`) has none
- **AND** fluid movement SHALL be expressed by the recipe's fluid port

### Requirement: Chemistry tree intermediates are ordered by producer

Every chemistry item SHALL have a producer, and every recipe input SHALL either
be world-generated or itself have a producer. The system SHALL fail the
reachability guard when a new chemistry item is unreachable.

#### Scenario: Intermediates are craftable, not conjured

- **GIVEN** a quest requires `benzene`
- **THEN** `benzene` SHALL have a producer recipe
- **AND** every item that recipe consumes SHALL also be obtainable
- **AND** `content_reachability_test` SHALL fail naming the item if not

#### Scenario: Chemistry does not consume stranded fluids

- **GIVEN** `oil` and `hydrogen` are recorded reachability debt with no producer
- **WHEN** a chemistry recipe is authored consuming either
- **THEN** the recipe SHALL be rejected in review, because it hangs a new
  progression root off an item the game cannot yield
- **AND** the rejection is not worked around by adding the item to
  `reachability_baseline.inc`

### Requirement: Chemistry fluids exist as both fluid and item

A chemistry fluid SHALL have a row in `fluids.csv` and an item row in
`items.csv`, and the two SHALL reference the same packed id.

#### Scenario: Fluid and item agree

- **GIVEN** a chemistry fluid `X` at `1111:11:N`
- **THEN** `fluids.csv` SHALL carry a row with `item_id: 1111:11:N`
- **AND** `items.csv` SHALL carry a row with the same id
- **AND** `Registry::validateReferences` (`Registry.cpp:206`) SHALL NOT report
  `item_id does not resolve in items.csv`, because that failure voids the whole
  catalog

#### Scenario: Bucket bridge is explicit

- **GIVEN** a chemistry fluid the player must carry as an item
- **THEN** `fluid_buckets.csv` SHALL carry a row mapping fluid → bucket item
- **AND** a fluid with no bucket row SHALL be deliberately excluded, because
  right-clicking a machine holding it does nothing rather than conjuring a bucket
  that does not exist

### Requirement: Chemistry quest line gates each intermediate

The system SHALL gate each chemistry intermediate behind its own producer quest
in `quest_graph.json`, and SHALL NOT expose a downstream chemical before its
upstream input is craftable.

#### Scenario: Downstream chemical is locked until upstream is craftable

- **GIVEN** a quest requires `styrene` and `styrene` derives from `toluene`
- **THEN** the `toluene` quest SHALL be a prerequisite of the `styrene` quest
- **AND** with `toluene` incomplete the `styrene` quest SHALL be LOCKED

#### Scenario: Section ordering inside the chemistry line

- **GIVEN** the `chemistry` quest section
- **THEN** it SHALL contain sub-sections in progression order: `chem_basics`,
  `chem_salts`, `chem_halogens`, `chem_organic`, `chem_plastics`,
  `chem_solvents`, `chem_silicon`, `chem_fuels`
- **AND** the sub-section order SHALL reflect the crafting dependency order, not
  alphabetical order