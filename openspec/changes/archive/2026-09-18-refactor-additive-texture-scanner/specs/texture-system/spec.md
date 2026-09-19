## ADDED Requirements

### Requirement: Texture Mapping Authoring

Texture mapping authoring tools SHALL use an additive, preservation-first workflow. Existing texture CSV rows, manual mappings, merge recipes, row order, and canonical pack pixels SHALL remain unchanged when scanning for new artwork.

#### Scenario: Dry-run scan
- **GIVEN** existing texture registries and canonical packs
- **WHEN** the scanner runs without `--apply`
- **THEN** it SHALL report proposed additions without modifying any registry or pack

#### Scenario: Existing mapping preservation
- **GIVEN** an existing manual item, block-face, or merge row
- **WHEN** a raw asset is scanned
- **THEN** the scanner SHALL preserve the existing row and SHALL NOT replace or reorder it

#### Scenario: New unambiguous artwork
- **GIVEN** a valid new raw artwork file with one exact registry match
- **WHEN** the scanner runs with `--apply`
- **THEN** it SHALL append a deterministic new pack cell and mapping without changing prior cells or rows

#### Scenario: Ambiguous artwork
- **GIVEN** artwork with no exact registry match, multiple matches, or ambiguous cell semantics
- **WHEN** the scanner runs
- **THEN** it SHALL report the asset as unmapped and SHALL NOT guess a gameplay mapping

#### Scenario: Idempotent scan
- **GIVEN** a scanner-applied addition
- **WHEN** the scanner runs again
- **THEN** it SHALL propose no duplicate cell, tile, mapping, or merge row
