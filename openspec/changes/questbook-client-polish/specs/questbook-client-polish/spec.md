## ADDED Requirements

### Requirement: Quest Book UI Polish
The client quest book SHALL use FlatBuffers, show unlock/completion notifications, and indicate era progression.

#### Scenario: Quest progress parsed via FlatBuffers
- **GIVEN** the client receives `QuestProgressUpdate` (msgType 19)
- **WHEN** `OnNetworkUpdate()` processes it
- **THEN** quest statuses SHALL be updated via FlatBuffers deserialization (no raw binary parsing)
- **AND** the handler SHALL use `GatewayMsg` enum constants, not hardcoded numeric ids

#### Scenario: Unlock notification displayed
- **GIVEN** the client receives `QuestUnlockNotification` (msgType 20)
- **WHEN** new quests become available
- **THEN** a visual notification SHALL appear
- **AND** the newly unlocked quests SHALL be highlighted in the quest list

#### Scenario: Completion notification displayed
- **GIVEN** the client receives `QuestCompletedNotification` (msgType 21)
- **WHEN** a quest is completed
- **THEN** a visual notification SHALL appear
- **AND** reward information SHALL be shown

#### Scenario: Manual completion button
- **GIVEN** an AVAILABLE quest is selected
- **WHEN** the player clicks "Complete" button in the detail view
- **THEN** a completion request SHALL be sent to SimulationCore via Gateway for validation
- **AND** upon server acceptance the local status SHALL update to COMPLETED

#### Scenario: Era tabs show progression and lock state
- **GIVEN** the quest book is open
- **THEN** era tabs SHALL show a completion ratio badge (e.g. "3/12")
- **AND** locked eras SHALL be dimmed/gray until unlocked
- **AND** newly unlocked eras SHALL show a brief visual effect
