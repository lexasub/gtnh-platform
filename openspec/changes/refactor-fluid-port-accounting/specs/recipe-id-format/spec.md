## MODIFIED Requirements

### Requirement: Recipe Item ID Format

The system SHALL accept three unambiguous formats for item identifiers in recipe
YAMLs, each detected by pattern, and SHALL resolve every resulting item ID
against the canonical `items.csv` registry. A recipe referring to an unknown
item SHALL fail validation rather than silently becoming item ID zero.

#### Scenario: Hierarchical prefix format
- **GIVEN** a recipe YAML with `item: 0:0:4`
- **WHEN** the parser processes the item field
- **THEN** the value is resolved via `ItemId::pack("0:0:4")`
- **AND** the resolved ID exists in `items.csv`
- **AND** the recipe loads and matches without ambiguity

#### Scenario: Hierarchical id is never silently zero
- **GIVEN** a recipe YAML with `item: 0:110:1`
- **WHEN** the parser processes the item field
- **THEN** the resolved item ID equals `ItemId::pack("0:110:1")`
- **AND** it is not zero
- **AND** the ID is validated against `items.csv`

#### Scenario: Flat numeric format (backward compat)
- **GIVEN** a recipe YAML with `item: 13`
- **WHEN** the parser detects a purely numeric string without colons
- **THEN** `ItemId::pack("13")` resolves according to the documented legacy
  compatibility rule
- **AND** the resulting ID is validated against `items.csv`

#### Scenario: String name format
- **GIVEN** a recipe YAML with `item: iron_ingot`
- **WHEN** the parser detects non-numeric characters without colons
- **THEN** `resolveItemName("iron_ingot")` resolves through the canonical
  `ItemRegistry::nameToId()`
- **AND** the recipe loads only if that name exists in `items.csv`

### Requirement: No Fallback Parsing

The parser SHALL NOT rely on try/catch for format detection and SHALL reject
unknown or unresolved item names/IDs.

#### Scenario: Explicit format detection
- **GIVEN** any `item:` field value
- **WHEN** the parser processes it
- **THEN** format is determined by pattern (`:` present → hierarchical, all
  digits → flat numeric, else → name)
- **AND** each format uses its dedicated conversion path
- **AND** an unresolved result is a validation error rather than zero
