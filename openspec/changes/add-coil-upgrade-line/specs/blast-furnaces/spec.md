## ADDED Requirements

### Requirement: Blast-furnace blocks resolve to placeable items

Every block id referenced by a `PatternLibrary.h` constant SHALL resolve to a
row in `items.csv`, so that a blast furnace can be built from blocks the player
can actually place.

#### Scenario: Coils and casings exist as items

- **GIVEN** `NICHROME_COIL_BLOCK_ID` is `ItemId::pack("1110:111:6")`
- **WHEN** the registry is loaded
- **THEN** a row for `1110:111:6` SHALL exist in `items.csv`
- **AND** that row SHALL have a recipe producer, so the player can craft it
- **AND** a test SHALL assert every constant in `PatternLibrary.h` resolves, so
  the gap cannot reopen

#### Scenario: Legacy controller aliases still match

- **GIVEN** `LEGACY_EBF_CONTROLLER_BLOCK_ID = 1003` and
  `LEGACY_HBF_CONTROLLER_BLOCK_ID = 1009`
- **WHEN** a pattern is matched against a legacy block id
- **THEN** the matcher SHALL still accept it (`PatternLibrary.cpp:166`)
- **AND** re-pointing the canonical constants SHALL NOT remove the legacy path

#### Scenario: Constant naming matches registry naming

- **GIVEN** the coil for Kanthal is spelled `KANHAL_COIL_BLOCK_ID` in code
- **THEN** the item it resolves to and the constant that names it SHALL use the
  same spelling
- **AND** a typo in either SHALL fail the constant-resolution test rather than
  silently mismatching

### Requirement: Coil ladder is a progression path

The system SHALL provide ten coil stages, each craftable, each strictly stronger
than the previous, and each gated behind the previous in the quest graph.

#### Scenario: Coil stage N+1 is locked until stage N completes

- **GIVEN** a coil ladder Kanthal → Nichrome → HSS-G → … → Awakened Draconium
- **THEN** the quest for stage N+1 SHALL list the quest for stage N as a
  prerequisite
- **AND** with stage N incomplete the stage N+1 quest SHALL be LOCKED

#### Scenario: Each coil raises a stated ceiling

- **GIVEN** a coil quest in the ladder
- **THEN** its description SHALL state what the upgrade buys — the heat ceiling
  it unlocks, the throughput change, or the alloy tier it enables
- **AND** the description SHALL NOT be a bare item name

#### Scenario: Casings scale with coil tier

- **GIVEN** casing tiers LV through UV
- **THEN** casing tier N SHALL be craftable from casing tier N-1
- **AND** a blast furnace pattern SHALL accept casings of the tier its coil
  requires, and SHALL reject lower tiers

### Requirement: Tiered glass ladder

The system SHALL provide nine glass tiers, each craftable only at the heat
ceiling the matching coil tier unlocks, and each quest SHALL state what the tier
buys rather than calling it "stronger glass".

#### Scenario: Glass tier N+1 is locked until coil N is craftable

- **GIVEN** `luv_glass` requires the Nichrome-tier heat ceiling
- **THEN** the Nichrome coil quest SHALL be a prerequisite of the `luv_glass`
      quest
- **AND** the crafting recipe for `luv_glass` SHALL declare the tier
      requirement that keeps it out of reach at a lower coil

#### Scenario: Producing machine is addressable

- **GIVEN** `vacuum_freezer` and `implosion_compressor` exist as craftable items
- **WHEN** a glass recipe declares `class: vacuum_freezer` or
      `class: implosion_compressor`
- **THEN** the block SHALL be present in `classByBlockId_`
- **AND** placing it SHALL let `findRecipesForMachine` return that recipe

#### Scenario: Glass quest states a rating, not a strength adjective

- **GIVEN** a quest for `uv_glass`
- **THEN** its description SHALL state the heat rating the tier provides
- **AND** it SHALL NOT describe the tier only as "stronger glass"
