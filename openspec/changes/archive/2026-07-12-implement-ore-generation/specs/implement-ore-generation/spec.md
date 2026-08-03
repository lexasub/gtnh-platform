## ADDED Requirements

### Requirement: Sinusoidal Ore Veins
The system SHALL generate ore veins using 3D sinusoidal noise.

#### Scenario: Ore generates in valid range
- **GIVEN** a chunk is generated
- **WHEN** the generator evaluates ore placement
- **THEN** iron ore generates between Y=0 and Y=64
- **AND** gold ore generates between Y=0 and Y=32
- **AND** diamond ore generates between Y=0 and Y=16

#### Scenario: Ore density follows noise threshold
- **GIVEN** a sinusoidal noise function
- **WHEN** noise value exceeds the ore's threshold
- **THEN** an ore block is placed
- **AND** surrounding blocks within the vein radius may also be ore

#### Scenario: Ore block persists correctly
- **GIVEN** an ore block was generated
- **WHEN** the chunk is saved to LMDB
- **THEN** the ore block_id is preserved on reload
