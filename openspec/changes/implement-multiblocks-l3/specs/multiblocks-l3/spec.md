## ADDED Requirements

### Requirement: Hatch Detection
The system SHALL resolve and use multiblock hatches.

#### Scenario: Active hatch scan on formation
- **GIVEN** a multiblock is formed (pattern matched)
- **WHEN** the controller is created
- **THEN** `findHatches()` SHALL resolve each pattern-relative hatch position to a world position
- **AND** SHALL create inventory containers on the `MultiblockController` per hatch role (ITEM_IN/OUT, FLUID_IN/OUT, ENERGY, MUFFLER)

#### Scenario: Hatch slots used by tick systems
- **GIVEN** an EBF/Large Boiler/LCR multiblock with hatches
- **WHEN** the tick system processes a recipe
- **THEN** inputs SHALL be read from ITEM_IN hatches and outputs SHALL be written to ITEM_OUT hatches

#### Scenario: Per-face hatch side config
- **GIVEN** a hatch block is wrenched
- **WHEN** side_config changes
- **THEN** the hatch SHALL respect the configured face for item/fluid/energy transfer

### Requirement: Hatch Contents Eject
The system SHALL eject hatch contents when a multiblock is dissociated.

#### Scenario: Items ejected on dissociation
- **GIVEN** a multiblock with items in hatch containers
- **WHEN** the multiblock is dissociated
- **THEN** the items SHALL spawn as world entities at the hatch positions

### Requirement: Client Multiblock Visuals
The client SHALL render a multiblock status window.

#### Scenario: Multiblock window opened
- **GIVEN** the client has received multiblock events or block entity updates
- **WHEN** the player interacts with a multiblock controller
- **THEN** a window SHALL show heat, recipe progress, and hatch contents
