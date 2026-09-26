# explosion-mechanics Specification

## Purpose
Explosion behavior on critical overheat: damage, spread, and server-side handling.
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

### Requirement: Explosion Blast Radius and Falloff
An explosion SHALL destroy a radius of blocks around the exploding machine, with
a falloff that shrinks the blast's reach with distance from the epicentre.

The geometry is defined by `HeatConstants` and SHALL NOT be inlined elsewhere:

| Constant | Value | Meaning |
|----------|-------|---------|
| `EXPLOSION_RADIUS` | 3 | outermost blast reach, in blocks, from the epicentre |
| `EXPLOSION_ANCHOR_REACH` | 2 | reach for multiblock controller anchors (`mb_id != 0`) |
| `EXPLOSION_SINGLEBLOCK_REACH` | 3 | reach for single-block machines (`mb_id == 0`) |

Distance SHALL be Euclidean (squared distance is compared directly, so no sqrt
is required), measured from the exploding machine's own position. Anchors are
reinforced and therefore take the shorter reach; this tier split is the falloff.

#### Scenario: Single-block machines within the radius are destroyed
- **GIVEN** a fused, CRITICAL multiblock anchor that explodes
- **AND** a single-block machine within `EXPLOSION_SINGLEBLOCK_REACH` (Euclidean) of the epicentre
- **WHEN** `ExplosionSystem::tick()` runs
- **THEN** that machine's block is cleared via `publishBlockChangedEvent` with `block_id = 0`, `meta = 0`
- **AND** that machine's ECS entity is removed

#### Scenario: Machines beyond the radius survive
- **GIVEN** a machine whose Euclidean distance from the epicentre exceeds its reach tier
- **WHEN** the explosion is processed
- **THEN** the machine is neither published nor destroyed

#### Scenario: Falloff is spherical, not cubical
- **GIVEN** a single-block machine at offset (2, 2, 2) from the epicentre, i.e. Euclidean distance sqrt(12) ≈ 3.46
- **WHEN** `EXPLOSION_SINGLEBLOCK_REACH = 3` is applied
- **THEN** the machine SURVIVES, because 3.46 > 3
- **AND** the blast SHALL NOT be implemented as a per-axis (cubical) test, which would wrongly destroy it

#### Scenario: Multiblock anchors take the shorter reach
- **GIVEN** a multiblock anchor at Euclidean distance `EXPLOSION_SINGLEBLOCK_REACH` from the epicentre
- **WHEN** the explosion is processed
- **THEN** the anchor SURVIVES, because `EXPLOSION_ANCHOR_REACH = 2 < 3`
- **AND** a single-block machine at the same distance is destroyed

#### Scenario: The blast does not chain
- **GIVEN** a machine destroyed by a blast is adjacent to another machine outside the radius
- **WHEN** `ExplosionSystem::tick()` runs
- **THEN** the machine outside the radius SHALL survive
- **AND** blast victims SHALL NOT be treated as epicentres, so the wave cannot relay outward one machine per hop

#### Scenario: Overlapping epicentres clear each block once
- **GIVEN** two exploded machines whose blast radii overlap on a third machine
- **WHEN** `ExplosionSystem::tick()` runs
- **THEN** the shared victim is destroyed
- **AND** its block is published exactly once

#### Scenario: The epicentre always clears its own block
- **GIVEN** a machine that explodes
- **WHEN** the blast is processed
- **THEN** the epicentre's own block is cleared regardless of the falloff tiers

### Requirement: Blast Visibility Is Limited to Loaded Machines
The blast SHALL destroy only blocks the simulation can resolve, and the
limitation SHALL be explicit rather than implied.

`ExplosionSystem` has no chunk or block repository: its only output channel is
`IEventPublisher::publishBlockChangedEvent`, and the only positions it can
enumerate come from the ECS registry. A blast therefore clears machines
carrying `MachineComponent` and `Position`. Plain (non-machine) blocks and
blocks in unloaded chunks are not reachable.

#### Scenario: Non-machine blocks inside the blast survive
- **GIVEN** an entity inside the blast radius that carries `Position` but no `MachineComponent`
- **WHEN** the explosion is processed
- **THEN** the entity SHALL NOT be destroyed
- **AND** no `publishBlockChangedEvent` SHALL be emitted for its position

#### Scenario: Single-block machines are blast targets, never epicentres
- **GIVEN** a single-block machine (`MachineComponent::mb_id == 0`) that is CRITICAL and fused
- **WHEN** `ExplosionSystem::tick()` runs
- **THEN** it SHALL NOT explode on its own fuse, matching the epicentre gate
- **AND** it SHALL still be destroyed by a neighbouring machine's blast
