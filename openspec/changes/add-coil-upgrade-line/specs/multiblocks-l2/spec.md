## ADDED Requirements

### Requirement: Exotic hatch types are addressable

The system SHALL support Data Access, Void Bus input/output, and Filtered
Beamline Output hatches as hatch types on multiblock part slots, and every
registered hatch block id SHALL map to a non-`NONE` `HatchType`.

#### Scenario: Registered hatch id maps to a real type

- **GIVEN** a hatch item id present in `items.csv`
- **WHEN** `hatchBlockIdToType` is called with it
- **THEN** the result SHALL NOT be `HatchType::NONE`
- **AND** a test SHALL assert this for every registered hatch id, because the
      switch has no default-raise and an unhandled id returns `NONE` — the hatch
      would occupy a pattern slot while connecting nothing, silently

#### Scenario: Void bus is distinguishable from an item bus

- **GIVEN** a void bus input hatch and a normal item input hatch
- **THEN** the two SHALL map to different `HatchType` values
- **AND** a machine's inventory reservation SHALL NOT be satisfied by a void bus,
      because a void bus consumes surplus rather than storing it

#### Scenario: Hatch kind is a property of the pattern slot

- **GIVEN** a multiblock pattern declaring an ITEM_IN hatch slot
- **WHEN** a void bus input hatch is placed at that offset
- **THEN** `findHatches` SHALL report it with the void-bus type
- **AND** the reservation logic SHALL treat it as a sink, not as item storage