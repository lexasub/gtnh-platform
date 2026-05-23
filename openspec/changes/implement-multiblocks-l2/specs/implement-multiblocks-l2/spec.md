## ADDED Requirements

### Requirement: Multiblock Pattern Library
The system SHALL support generic multiblock pattern matching.

#### Scenario: EBF pattern detected
- **GIVEN** a player builds a 3x3x4 hollow structure with controller block
- **WHEN** the last pattern block is placed
- **THEN** SimulationCore detects the EBF pattern
- **AND** creates a MultiblockController ECS entity
- **AND** publishes sim.multiblock.created

### Requirement: Multiblock Dissociation
The system SHALL detect and handle multiblock destruction.

#### Scenario: Anchor block broken disassembles multiblock
- **GIVEN** a multiblock exists with a controller at the anchor position
- **WHEN** the anchor block is broken (becomes air)
- **THEN** SimulationCore destroys the MultiblockController
- **AND** clears mb_id from all pattern blocks via ChunkStore
- **AND** publishes sim.multiblock.destroyed

### Requirement: Spatial Queries
The system SHALL support spatial queries for multiblocks and entities.

#### Scenario: Find multiblocks in radius
- **GIVEN** a world position and radius
- **WHEN** SpatialIndex is queried
- **THEN** it returns all multiblocks with bounding boxes intersecting the query region
