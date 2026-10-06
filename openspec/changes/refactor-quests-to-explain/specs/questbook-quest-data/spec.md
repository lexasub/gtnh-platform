## ADDED Requirements

### Requirement: Explaining quests

The system SHALL support a requirement kind `explain`: a quest that carries no
item requirement, is completed by the player explicitly, and whose payload is
prose.

#### Scenario: Explain quest has no item requirement

- **GIVEN** a quest whose only requirement has `kind: explain`
- **THEN** the quest SHALL NOT resolve to a packed item id
- **AND** the quest SHALL never auto-complete on inventory or craft events

#### Scenario: Explain quest completes manually

- **GIVEN** an `explain` quest whose prerequisites are all COMPLETED
- **THEN** the quest SHALL be AVAILABLE
- **AND** it SHALL transition to COMPLETED only when the player activates it
- **AND** it SHALL behave identically to an `auto_complete: false` quest

#### Scenario: Explain quest is locked behind prerequisites

- **GIVEN** an `explain` quest with an incomplete prerequisite
- **THEN** the quest SHALL be LOCKED
- **AND** its prose SHALL NOT be visible while locked

#### Scenario: Explain quest grants its reward

- **GIVEN** an `explain` quest with a reward
- **WHEN** the player completes it manually
- **THEN** the reward SHALL be granted through the existing completion flow
- **AND** the quest SHALL NOT be re-completable

### Requirement: Quest prose columns

`quests.csv` SHALL carry optional `hint` and `lore` columns, and a missing or
empty value SHALL load as empty rather than fail.

#### Scenario: Hint renders before completion

- **GIVEN** a quest with a non-empty `hint`
- **WHEN** the quest book renders it
- **THEN** the hint SHALL be visible to the player before the quest is completed

#### Scenario: Lore is available but does not clutter

- **GIVEN** a quest with a non-empty `lore`
- **THEN** the lore SHALL NOT be shown expanded by default
- **AND** the player SHALL be able to expand it

#### Scenario: Legacy CSV without the columns still loads

- **GIVEN** a `quests.csv` with no `hint` or `lore` columns
- **WHEN** the quest library loads it
- **THEN** it SHALL log no error
- **AND** every quest SHALL have an empty hint and empty lore

### Requirement: Quest sub-steps

A quest SHALL support an ordered list of sub-steps, and SHALL complete only when
all sub-steps are satisfied in order.

#### Scenario: Sub-steps gate completion

- **GIVEN** a quest with three sub-steps where only the first is satisfied
- **THEN** the quest SHALL NOT be completable
- **AND** the UI SHALL show one of three sub-steps complete

#### Scenario: Sub-step order is enforced

- **GIVEN** a quest whose second sub-step is satisfied but whose first is not
- **THEN** the second sub-step SHALL NOT count as satisfied
- **AND** the quest SHALL remain incomplete

#### Scenario: Sub-step completion is recorded individually

- **GIVEN** a sub-step that has been satisfied
- **WHEN** the player loses the relevant item
- **THEN** the recorded sub-step SHALL remain satisfied, because completion is a
  latch and not a live inventory query

#### Scenario: Sub-steps coexist with prerequisites

- **GIVEN** a quest with both sub-steps and prerequisite quests
- **THEN** the quest SHALL be LOCKED until its prerequisites complete
- **AND** its sub-steps SHALL only be evaluable once it is AVAILABLE

### Requirement: Describing why, not only what

Every authored quest description SHALL state the reason the player would want
the thing, not only the instruction to obtain it.

#### Scenario: Machine quest explains its purpose

- **GIVEN** a quest for `thermal_centrifuge_lv`
- **THEN** its description SHALL state what the machine is for and what it
  enables
- **AND** the description SHALL NOT be a bare "Craft a thermal centrifuge"

#### Scenario: Rewriting preserves quest identity

- **GIVEN** a quest whose description is rewritten in explaining style
- **THEN** its id SHALL be unchanged
- **AND** its `quest_graph.json` prerequisite edges SHALL be unchanged
- **AND** any saved completion state in MetaDB SHALL still apply to it