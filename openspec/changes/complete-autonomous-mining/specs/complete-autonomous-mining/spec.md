## ADDED Requirements

### Requirement: Autonomous Mining Persistence
The system SHALL persist drill state across restarts.

#### Scenario: Drill state saved on tick
- **GIVEN** a drill is actively mining
- **WHEN** it completes a mining step
- **THEN** DrillComponent state (position, progress, output buffer) is saved to EntityStateStore
- **AND** on SimulationCore restart, the drill resumes from saved state

### Requirement: Item Pipe Integration
The system SHALL support automatic output from drills into item pipes.

#### Scenario: Drill outputs to connected pipe
- **GIVEN** a drill with items in its output buffer
- **WHEN** a face with OUTPUT role is connected to an item pipe
- **THEN** the drill pushes items into the pipe network automatically

### Requirement: Drill Client UI
The system SHALL provide a client UI for drill monitoring.

#### Scenario: Drill window shows status
- **GIVEN** a player opens a drill block
- **THEN** the UI shows current position, progress bar, energy level, and output buffer
