## ADDED Requirements

### Requirement: Quest Data Model
The system SHALL support a quest progression system.

#### Scenario: Quests are organized in eras
- **GIVEN** the quest data
- **THEN** quests are grouped into eras: Vagrant, Apprentice, Expert, Administrator
- **AND** eras contain sections (Foundation, Electric Tools, Machine Config, Transport...)
- **AND** sections contain individual quests

### Requirement: Quest Completion Detection
The system SHALL automatically detect quest completion.

#### Scenario: Craft completion detected
- **GIVEN** a quest requires crafting a specific item
- **WHEN** the player crafts that item
- **THEN** the quest is marked as auto-completed

#### Scenario: Manual completion allowed
- **GIVEN** a quest without auto-detection
- **WHEN** the player marks it complete
- **THEN** the quest is marked as manually completed

### Requirement: Quest Unlock Logic
The system SHALL unlock quests based on a DAG.

#### Scenario: Parent quests unlock children
- **GIVEN** a quest with dependencies
- **WHEN** all parent quests are completed
- **THEN** the child quest becomes available
