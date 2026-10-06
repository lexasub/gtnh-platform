## ADDED Requirements

### Requirement: Declined tech trees stay declined

The quest book SHALL NOT require items or explain mechanics belonging to a tech
tree the project has declined. A declined area SHALL NOT be re-opened without an
explicit, recorded reversal.

#### Scenario: No quests for declined trees

- **GIVEN** a quest whose description explains combat, farming, pollination,
      spaceflight or thaumaturgy
- **THEN** the quest SHALL NOT be added to the quest book
- **AND** the mechanic SHALL NOT be implemented, because none of those trees has
      a capability in `openspec/specs/` or any content in the registry

#### Scenario: Reversal is explicit

- **GIVEN** a proposal to implement a declined tree
- **THEN** it SHALL first record the reversal in the content backlog change,
      stating what changed in the project's direction
- **AND** it SHALL carry its own spec delta introducing the capability
- **AND** the reversal SHALL NOT happen silently inside an unrelated change

### Requirement: Pillars are built one at a time

Transport, AE2-style storage networks, and nuclear SHALL each be implemented
under their own proposal with their own spec delta, and SHALL NOT be added as
tasks to an in-flight content change.

#### Scenario: New pillar gets its own proposal

- **GIVEN** a decision to implement transport
- **THEN** it SHALL be implemented under its own `openspec/changes/` proposal
      with a spec delta for the capability it introduces
- **AND** it SHALL NOT appear as a task inside `add-chemistry-progression` or any
      other sibling change, because bundling a new pillar into a content change
      hides that a pillar is new

#### Scenario: Pillars do not imply each other

- **GIVEN** the transport pillar is in scope
- **THEN** that SHALL NOT imply AE2 networks or nuclear are in scope
- **AND** each SHALL be decided on its own merits

#### Scenario: Blocked pillar records its dependency

- **GIVEN** nuclear depends on the chemistry recipe tree and on reactor
      multiblocks
- **WHEN** the nuclear proposal is drafted
- **THEN** it SHALL state those dependencies
- **AND** starting it before the chemistry tree lands SHALL be recorded as
      taking on debt, not silently assumed away

### Requirement: Deferred content is recorded, not dropped

A GTNH area identified during research but not implemented SHALL be recorded in
the content backlog with its quest-line source, its size, and its disposition:
in scope, declined, or pending.

#### Scenario: Every identified area has a disposition

- **GIVEN** a GTNH area surfaced during questbook research
- **THEN** it SHALL appear in the backlog with one of: in scope (future change),
      declined, or pending a size call
- **AND** it SHALL NOT be silently omitted, so a later reader can tell an
      intentional decision from an oversight

#### Scenario: Dropped-by-dependency is explicit

- **GIVEN** an area whose only content source is a declined tree
- **THEN** it SHALL be marked as effectively dropped, with the dependency named
- **AND** it SHALL NOT be listed as in scope while depending on declined content