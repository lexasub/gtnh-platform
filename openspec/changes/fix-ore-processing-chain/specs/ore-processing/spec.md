## ADDED Requirements

### Requirement: Item String Names in Recipes
All item references in recipe YAML files SHALL use string names resolved via `ItemRegistry::nameToId()`. Hardcoded numeric item IDs SHALL NOT be used.

#### Scenario: String name resolves correctly at runtime
- **GIVEN** a recipe YAML file with `item: iron_ore`
- **WHEN** `RecipeManager::parseYamlInputItem()` parses it
- **THEN** `stoi()` fails on the string
- **AND** `resolveItemName("iron_ore")` calls `ItemRegistry::nameToId("iron_ore")`
- **AND** returns the correct packed uint16_t (32768)

#### Scenario: Numeric ID is rejected
- **GIVEN** a recipe YAML file uses `item: 3` (hardcoded numeric)
- **WHEN** the recipe file is loaded
- **THEN** it SHALL be treated as a bug — the parser accepts it but produces wrong item
- **AND** no recipe SHALL use numeric IDs after this change is complete

### Requirement: Complete Ore Processing Recipes
The system SHALL provide recipes for all ore types across macerator, furnace, and compressor.

#### Scenario: Iron ore full chain
- **GIVEN** iron ore is placed in a macerator
- **WHEN** the macerator recipe completes
- **THEN** 2 crushed_iron are produced
- **AND** crushed_iron in a furnace produces iron_ingot
- **AND** iron_ingot in a compressor produces iron_plate
- **AND** each step uses string item names only

#### Scenario: All ore types have complete chains
- **GIVEN** any registered ore type (copper, tin, lead, silver, zinc, gold, iron)
- **WHEN** processed through macerator→furnace→compressor
- **THEN** each step produces the correct output item
- **AND** all item references are string names

### Requirement: Item Registry Coverage
The item registry SHALL define all items needed for the ore processing chain before recipes reference them.

#### Scenario: Crushed ores registered
- **GIVEN** the ore processing chain requires crushed ore types
- **WHEN** `data/registry/items.csv` is loaded
- **THEN** crushed_iron, crushed_gold, crushed_copper, crushed_tin, crushed_lead, crushed_silver, crushed_zinc SHALL exist
- **AND** each has a unique `ItemId::pack()` value with no collisions

#### Scenario: Metal plates registered
- **GIVEN** the compressor produces plates from ingots
- **WHEN** `data/registry/items.csv` is loaded
- **THEN** iron_plate, gold_plate, copper_plate, tin_plate, lead_plate, silver_plate, zinc_plate SHALL exist
- **AND** each has a unique `ItemId::pack()` value with no collisions

### Requirement: Recipe Correctness
The system SHALL have bug-free recipes verified against the item registry.

#### Scenario: Compressor produces distinct output
- **GIVEN** a compressor recipe for iron
- **WHEN** iron_ingot is the input
- **THEN** the output is iron_plate (different item_id from input)

#### Scenario: Macerator per-ore outputs
- **GIVEN** a macerator contains any ore type
- **WHEN** the recipe completes
- **THEN** the output item_id matches the crushed variant of that specific ore
- **AND** not a generic dust ID shared across ore types

### Requirement: Item Pipe Transport Integration
The ore processing chain SHALL work with item pipe transport between machines.

#### Scenario: Items flow between chained machines
- **GIVEN** a macerator, furnace, and compressor are placed in sequence
- **AND** item pipes connect their output→input
- **WHEN** ore is placed in the macerator
- **THEN** crushed ore flows to the furnace input
- **AND** ingot flows to the compressor input
- **AND** plate is produced at the compressor output
- **AND** each item uses string-based IDs throughout the pipe network
