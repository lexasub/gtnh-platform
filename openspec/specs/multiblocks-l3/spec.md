# multiblocks-l3 Specification

## Purpose
TBD - created by archiving change implement-multiblocks-l3. Update Purpose after archive.
## Requirements
### Requirement: Hatch Detection
The system SHALL resolve and use multiblock hatches.

#### Scenario: Active hatch scan on formation
- **GIVEN** a multiblock is formed (pattern matched)
- **WHEN** the controller is created
- **THEN** `findHatches()` SHALL resolve each controller-relative hatch position to a world position (`world_pos = controller_pos + HatchDef offset`)
- **AND** SHALL create inventory containers on the `MultiblockController` per hatch role (ITEM_IN/OUT, FLUID_IN/OUT, ENERGY, MUFFLER)

#### Scenario: Hatch block identification
- **GIVEN** hatch blocks are declared in the pattern
- **WHEN** a hatch block is placed at a declared hatch position
- **THEN** `hatchBlockIdToType()` SHALL map the hatch block ID to its role
- **AND** hatch block IDs SHALL use the hierarchical items.csv format
- **AND** until `items.csv`/`machines.yaml` is updated, a placeholder ID with a TODO SHALL be used

#### Scenario: Hatch slots used by tick systems
- **GIVEN** an EBF or LCR multiblock with ITEM_IN and ITEM_OUT hatches
- **WHEN** the tick system processes a recipe
- **THEN** inputs SHALL be read from the ITEM_IN hatch slot range and outputs SHALL be written to the ITEM_OUT hatch slot range (via `getInputSlotRange`/`getOutputSlotRange`)
- **AND** a Large Boiler SHALL keep FLUID hatches, with fuel read from the controller container

#### Scenario: Per-face hatch side config
- **GIVEN** a hatch block is wrenched
- **WHEN** side_config changes
- **THEN** the hatch SHALL respect the configured face for item/fluid/energy transfer

### Requirement: Block-Break Inventory Guard
The system SHALL return multiblock contents to the breaking player and refuse to break when they do not fit.

#### Scenario: Contents returned on block break
- **GIVEN** a multiblock with items in hatch containers or the controller container
- **WHEN** a block of the multiblock (anchor or hatch) is broken
- **THEN** the contents SHALL be moved to the breaking player's inventory
- **AND** if the contents do not fit in the player's inventory, the block SHALL NOT break
- **AND** no item-entity spawn is required

### Requirement: Multiblock State Persistence
The system SHALL persist multiblock inventory contents.

#### Scenario: Hatch contents survive chunk unload
- **GIVEN** a multiblock with items in hatch containers
- **WHEN** the multiblock state is serialized via `serializeMultiblock()`
- **THEN** the hatch/container contents SHALL be included in `MultiblockState`
- **AND** restored on deserialization so no items are lost on chunk unload

### Requirement: Client Multiblock Visuals
The client SHALL render a multiblock status window.

#### Scenario: Multiblock window opened
- **GIVEN** the client has received multiblock events or block entity updates
- **WHEN** the player interacts with a multiblock controller
- **THEN** a window SHALL show heat, recipe progress, and hatch contents

