## MODIFIED Requirements

### Requirement: Electric Tools — Wrench Side Config
The system SHALL support wrench-based machine side configuration.

#### Scenario: Wrench raycast detects face
- **GIVEN** the player is looking at a machine
- **WHEN** the player presses G key
- **THEN** the client determines the targeted face via raycast
- **AND** sends ToolAction(WRENCH_CYCLE, pos, face)

#### Scenario: Side config saved to EntityStateStore
- **GIVEN** a machine's side_config was cycled
- **WHEN** the new config is applied
- **THEN** SimulationCore saves the side_config via EntityStateStore RPC
- **AND** publishes machine.config.updated on MessageRouter

### Requirement: Electric Tools — Battery Buffers
The system SHALL support battery buffer blocks for charging electric tools.

#### Scenario: Battery buffer charges tools
- **GIVEN** a battery buffer block is connected to PipeNetwork energy
- **WHEN** a tool with EnergyStorage is placed in its slot
- **THEN** each tick charges the tool up to its capacity
- **AND** stops when buffer energy is depleted

### Requirement: PipeNetwork — Side Config Routing
The system SHALL respect machine side roles during pipe routing.

#### Scenario: BFS respects side roles
- **GIVEN** a machine has side_config set
- **WHEN** PipeNetwork BFS discovers connections
- **THEN** it only routes items/energy/fluid through faces with matching roles
