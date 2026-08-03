# explosion-mechanics Specification

## Purpose
TBD - created by archiving change implement-explosion-mechanics. Update Purpose after archive.
## Requirements
### Requirement: Explosion on Critical Overheat
The system SHALL destroy machines that remain at CRITICAL overheat for a sustained duration.

#### Scenario: Machine explodes after critical overheat duration
- **GIVEN** a machine with `OverheatComponent.state == CRITICAL`
- **AND** `OverheatComponent.ticks_at_critical` is preserved across ticks (HeatTransferSystem Pass 2 does not reset it)
- **WHEN** `ExplosionSystem::tick()` runs for `HeatConstants::EXPLOSION_DELAY_TICKS = 60` consecutive ticks
- **THEN** the block at the machine's position is destroyed (set to air via `publishBlockChangedEvent`)
- **AND** the ECS entity is removed

#### Scenario: Warning below critical ticks
- **GIVEN** a machine with `ticks_at_critical < 60`
- **WHEN** `ExplosionSystem::tick()` runs
- **THEN** the ticks counter increments but the machine is NOT destroyed

#### Scenario: Recovery below warning threshold resets the countdown
- **GIVEN** a machine whose heat ratio drops below the warning threshold before reaching 60 ticks
- **WHEN** `HeatTransferSystem::tick()` Pass 2 runs
- **THEN** `OverheatComponent` is removed from the entity
- **AND** the explosion countdown is lost

