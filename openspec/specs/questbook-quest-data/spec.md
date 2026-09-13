# questbook-quest-data Specification

## Purpose
Quest reward data loaded from JSON quest definitions.
## Requirements
### Requirement: Quest reward data from JSON
The system SHALL load quest rewards from a dedicated `data/quests/quest_rewards.json` file rather than the flat `reward_item`/`reward_count` columns in `quests.csv`.

#### Scenario: Rewards loaded from JSON
- **GIVEN** `data/quests/quest_rewards.json` exists and is well-formed
- **WHEN** the client quest book loads quest data
- **THEN** each quest's rewards SHALL be read from the JSON file, keyed by quest id
- **AND** item rewards SHALL pack their `item` spec string via `ItemId::pack` for icon lookup

#### Scenario: Multiple rewards per quest
- **GIVEN** a quest with a `rewards` array containing more than one entry
- **THEN** all entries SHALL be available for rendering and reward resolution

#### Scenario: Choice rewards
- **GIVEN** a quest with a `choice_of` array
- **THEN** all choice options SHALL be available for rendering
- **AND** a quest SHALL define either `rewards` or `choice_of`, not both (validated)

#### Scenario: Missing or malformed JSON
- **GIVEN** `quest_rewards.json` is missing or malformed
- **THEN** the system SHALL log a warning
- **AND** quests SHALL render with no reward rows rather than fail

### Requirement: Quest requirement data from JSON
The system SHALL load quest requirements from a dedicated `data/quests/quest_requirements.json` file rather than the flat `detect_type`/`detect_target`/`target_count` columns in `quests.csv`.

#### Scenario: Requirements loaded from JSON
- **GIVEN** `data/quests/quest_requirements.json` exists and is well-formed
- **WHEN** the client quest book loads quest data
- **THEN** each quest's requirements SHALL be read from the JSON file, keyed by quest id
- **AND** each requirement's `item` spec string SHALL pack via `ItemId::pack` for icon lookup

#### Scenario: Requirement kinds
- **GIVEN** a quest with requirements of kinds `craft`, `obtain`, `place`, or `machine`
- **THEN** each requirement SHALL carry its kind
- **AND** a `machine` requirement SHALL carry a `machine` item spec

#### Scenario: Consume flag
- **GIVEN** a requirement with `consume: true`
- **THEN** the requirement SHALL be marked as taken on completion
- **AND** a requirement with `consume: false` SHALL be marked as kept

#### Scenario: Missing or malformed JSON
- **GIVEN** `quest_requirements.json` is missing or malformed
- **THEN** the system SHALL log a warning
- **AND** quests SHALL render with no requirement rows rather than fail

### Requirement: Quest auto-completion control
The system SHALL let each quest opt in to instant completion when its requirement is met, or require manual completion via the Complete button.

#### Scenario: Auto-complete quest
- **GIVEN** a quest with `auto_complete: true` whose requirement is met and prerequisites are satisfied
- **THEN** the quest SHALL transition to COMPLETED immediately
- **AND** rewards SHALL be granted through the existing completion flow

#### Scenario: Manual-complete quest
- **GIVEN** a quest with `auto_complete: false` whose requirement is met
- **THEN** the quest SHALL transition to AVAILABLE (not COMPLETED)
- **AND** the quest SHALL complete only when the player presses the Complete button

#### Scenario: Auto-complete default
- **GIVEN** a quest with no `auto_complete` field in `quest_requirements.json`
- **THEN** the quest SHALL behave as `auto_complete: true`

### Requirement: Quest machine-output detection
The system SHALL detect quests of kind `machine` — obtaining an item from a machine of a specified type.

#### Scenario: Item obtained from machine
- **GIVEN** a quest with a requirement of kind `machine` for item X in machine Y
- **WHEN** a player takes item X from the output of a machine of type Y
- **THEN** the quest SHALL be evaluated for completion through the machine-output detection path

#### Scenario: Machine attribution fallback
- **GIVEN** a machine-output take event lacks the machine type
- **THEN** the quest SHALL fall back to INVENTORY-style detection (item held ≥ required count)

