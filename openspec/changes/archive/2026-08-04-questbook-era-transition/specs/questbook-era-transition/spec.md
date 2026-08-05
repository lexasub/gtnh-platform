## ADDED Requirements

### Requirement: Era Transition
The system SHALL track era completion and transition between eras.

#### Scenario: Era complete when all quests completed
- **GIVEN** a player has quests in Era::VAGRANT
- **WHEN** all quests in that era reach COMPLETED status
- **THEN** `QuestGraph::IsEraComplete()` SHALL return true (backed by `BuildQuestEraMap()`)
- **AND** a notification SHALL be sent to the client (first quest in next era unlocked)

#### Scenario: Era transition triggers next era
- **GIVEN** Era::VAGRANT is complete
- **WHEN** any quest from Era::APPRENTICE becomes unlocked
- **THEN** the client SHALL show the new era tab as active/available
