## ADDED Requirements

### Requirement: Advanced Quest Detection
The system SHALL detect quest completion for all supported detection types.

#### Scenario: Tool charge completion
- **GIVEN** a quest requires charging a tool (`DetectionType::TOOL_CHARGED`, detectTarget = tool_id)
- **WHEN** the player tool reaches full charge
- **THEN** the quest SHALL be marked COMPLETED

#### Scenario: Side config completion
- **GIVEN** a quest requires configuring a machine side (`DetectionType::SIDE_CONFIGURED`)
- **WHEN** the player wrenches a machine face
- **THEN** the quest SHALL be marked COMPLETED

### Requirement: QuestGraph-Backed Unlock Logic
The system SHALL evaluate unlocks via QuestGraph and publish unlock events.

#### Scenario: Parent quests unlock children via QuestGraph
- **GIVEN** a quest with prerequisites
- **WHEN** all parent quests are completed
- **THEN** `QuestGraph::CanComplete()` SHALL return true
- **AND** `QuestGraph::NewlyAvailable()` SHALL include this quest in the next unlock batch
- **AND** a `QuestUnlocked` event SHALL be published on `quest.unlocked` topic

#### Scenario: DAG evaluated from QuestGraph not inline
- **GIVEN** QuestManager checks completion
- **WHEN** prerequisites need evaluation
- **THEN** QuestManager SHALL evaluate prerequisites via `QuestGraph::CanComplete()` instead of inline loops
