## ADDED Requirements

### Requirement: Complete Ore Processing Recipes
The system SHALL provide recipes for all ore types across macerator, furnace, and compressor.

#### Scenario: Iron ore processing chain
- **GIVEN** iron ore is placed in a macerator
- **WHEN** the recipe completes
- **THEN** 2 crushed_iron are produced
- **AND** crushed_iron in a furnace produces iron_ingot
- **AND** iron_ingot in a compressor produces iron_plate

#### Scenario: All ore types have complete chains
- **GIVEN** any registered ore type (copper, tin, lead, silver, zinc, gold, iron)
- **WHEN** processed through macerator→furnace→compressor
- **THEN** each step produces the correct output item

### Requirement: Recipe Correctness
The system SHALL have bug-free recipes with correct input/output item IDs.

#### Scenario: Compressor produces distinct output
- **GIVEN** a compressor recipe for iron
- **WHEN** iron_ingot is the input
- **THEN** the output is iron_plate (different item_id from input)
