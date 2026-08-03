# recipe-id-format Specification

## Purpose

Define the accepted formats for item identifiers in recipe YAML files and how the RecipeManager resolves them to packed `uint16_t` item ids. Recipes may reference items by hierarchical prefix notation (`0:0:13`), flat numeric ids (`13`, backward compat), or string names (`iron_ingot`); the parser SHALL detect the format by pattern and resolve all three to the identical packed id with no ambiguity.

## Requirements
### Requirement: Recipe Item ID Format
The system SHALL accept three unambiguous formats for item identifiers in recipe YAMLs, each detected by pattern.

#### Scenario: Hierarchical prefix format
- **GIVEN** a recipe YAML with `item: 0:0:4`
- **WHEN** the parser processes the item field
- **THEN** the value SHALL be resolved via `ItemId::pack("0:0:4")` rather than parsed as a flat number
- **AND** the recipe SHALL load and match at runtime with zero ambiguity

#### Scenario: Hierarchical id is never silently zero
- **GIVEN** a recipe YAML with `item: 0:110:1`
- **WHEN** the parser processes the item field
- **THEN** the resolved `item_id` SHALL equal `ItemId::pack("0:110:1")`
- **AND** SHALL NOT be `0` (the value `std::stoi` would return by stopping at the first colon)

#### Scenario: Flat numeric format (backward compat)
- **GIVEN** a recipe YAML with `item: 13`
- **WHEN** the parser detects a purely numeric string without colons
- **THEN** `ItemId::pack("13")` SHALL return the same uint16_t as `ItemId::pack("0:0:13")`
- **AND** the recipe SHALL load and match identically

#### Scenario: String name format
- **GIVEN** a recipe YAML with `item: iron_ingot`
- **WHEN** the parser detects non-numeric characters without colons
- **THEN** `resolveItemName("iron_ingot")` SHALL resolve via `ItemRegistry::nameToId()`
- **AND** the recipe SHALL load and match at runtime

### Requirement: No Fallback Parsing
The parser SHALL NOT rely on try/catch for format detection.

#### Scenario: Explicit format detection
- **GIVEN** any `item:` field value
- **WHEN** the parser processes it
- **THEN** format SHALL be determined by pattern (`:` present → hierarchical, all digits → flat numeric, else → name)
- **AND** each SHALL use its dedicated conversion path

