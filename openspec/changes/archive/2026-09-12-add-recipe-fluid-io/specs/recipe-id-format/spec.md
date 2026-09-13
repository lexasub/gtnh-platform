## MODIFIED Requirements

### Requirement: Recipe Item ID Format
The system SHALL accept three unambiguous formats for item identifiers in recipe
YAMLs, each detected by pattern, and SHALL resolve every resulting item ID
against the canonical `items.csv` registry. A recipe referring to an unknown
item SHALL fail validation rather than silently becoming item ID zero. The same
resolution rules SHALL apply to `fluid:` names inside `fluid_inputs` and
`fluid_outputs`, with the additional constraint that the resolved id MUST have
a `fluids.csv` properties row (see recipe-fluid-io).

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

#### Scenario: Fluid name resolves through the same registry
- **GIVEN** a recipe YAML with `fluid_inputs: [{ fluid: water, amount: 1000 }]`
- **WHEN** the parser resolves `water`
- **THEN** it uses the item-name resolution path
- **AND** additionally requires a `fluids.csv` row for the packed id
- **AND** the recipe loads only when both validations pass
