# blast-furnaces Specification

## Purpose
TBD - created by archiving change separate-hbf-ebf-blast-furnaces. Update Purpose after archive.
## Requirements
### Requirement: Separate blast-furnace energy domains
The platform SHALL model EBF as an EU-powered blast furnace and HBF as a HEAT/HU-powered blast furnace.

#### Scenario: EBF uses electricity
- **GIVEN** a formed EBF with a physical ENERGY hatch
- **WHEN** an EBF dust recipe is processed
- **THEN** EU SHALL be requested and consumed through the ENERGY hatch
- **AND** heat/HU SHALL not be required for progress

#### Scenario: HBF uses heat
- **GIVEN** a formed HBF with sufficient heat/HU
- **WHEN** an HBF dust recipe is processed
- **THEN** heat/HU SHALL be consumed
- **AND** EU SHALL not be requested or consumed

### Requirement: Blast-furnace dust recipes
The platform SHALL provide canonical dust-to-ingot recipes for both furnace families.

#### Scenario: Iron dust smelting
- **GIVEN** one `iron_dust` in the ITEM_IN hatch
- **WHEN** the selected furnace has sufficient domain energy
- **THEN** the input SHALL be consumed exactly once
- **AND** two `iron_ingot` items SHALL be written to ITEM_OUT

#### Scenario: Domain mismatch is rejected
- **GIVEN** an EBF recipe declared for EU
- **WHEN** it is assigned to a HEAT-only HBF
- **THEN** RecipeManager SHALL reject the mismatch
- **AND** the furnace SHALL not advance progress

### Requirement: Shared multiblock lifecycle
The platform SHALL form and dismantle EBF and HBF through the existing multiblock controller lifecycle.

#### Scenario: Controller-last formation
- **GIVEN** all casing, coil, and physical hatch blocks are placed
- **WHEN** the controller is placed last
- **THEN** the correct furnace pattern SHALL form
- **AND** the controller SHALL expose ITEM_IN and ITEM_OUT ranges
- **AND** EBF SHALL expose its physical ENERGY hatch endpoint

