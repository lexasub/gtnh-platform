## ADDED Requirements

### Requirement: FlatBuffers quest parsing with enum constants
The client quest book SHALL deserialize quest updates via FlatBuffers and SHALL reference quest message types through `GatewayMsg` enum constants, never hardcoded numeric ids.

#### Scenario: Quest progress update parsed via FlatBuffers
- **GIVEN** the client receives `QuestProgressUpdate` (msgType 20)
- **WHEN** `OnNetworkUpdate()` processes it
- **THEN** quest statuses SHALL be updated via `flatbuffers::GetRoot<Protocol::QuestProgressUpdate>` on a verified buffer (no raw byte parsing)
- **AND** the dispatch SHALL use `GatewayMsg::kQuestProgressUpdate`

#### Scenario: Wire schema matches live constants
- **GIVEN** `gateway.fbs` declares the quest payload union entries
- **THEN** `QuestProgressUpdate` / `QuestUnlockNotification` / `QuestCompletedNotification` SHALL be numbered 20/21/22 to match the live wire constants in `gateway.h` and `NetClient.h`

### Requirement: Unlock notification
The client SHALL show a visual notification when new quests become available and SHALL highlight the newly unlocked quests.

#### Scenario: Unlock notification displayed
- **GIVEN** the client receives `QuestUnlockNotification` (msgType 20)
- **WHEN** new quests become available
- **THEN** a visual notification SHALL appear (e.g. banner/toast)
- **AND** the newly unlocked quest IDs SHALL be highlighted in the quest list

### Requirement: Completion notification
The client SHALL show a visual notification with reward info when a quest is completed.

#### Scenario: Completion notification displayed
- **GIVEN** the client receives `QuestCompletedNotification` (msgType 21)
- **WHEN** a quest is completed
- **THEN** a visual notification SHALL appear
- **AND** the reward item/count from the notification SHALL be shown

### Requirement: Era completion badges
Each era tab SHALL display its completion ratio and SHALL refresh as quest statuses arrive.

#### Scenario: Era tab badge
- **GIVEN** the quest book is open
- **THEN** each era tab SHALL render a completion badge (e.g. "3/12")
- **AND** the badge SHALL update as quest statuses arrive

### Requirement: Era lock state
Locked eras SHALL be visually distinct and SHALL NOT be selectable. The lock is released only when the server signals the era unlocked.

#### Scenario: Era locked until server confirms unlock
- **GIVEN** the quest book is open
- **WHEN** the server has not yet signaled an era as unlocked
- **THEN** the era tab SHALL be dimmed/gray and SHALL NOT be selectable (safe default: locked until told otherwise)
- **AND** the completion-ratio badge SHALL still be derived client-side from received quest statuses and SHALL render while locked
- **AND** when the server signals an era unlock (era-transition notification, per `questbook-era-transition`) the tab SHALL become selectable and SHALL show a brief visual effect

### Requirement: No client write-path for quest status
The client SHALL NOT send quest progress writes over `meta_db.quest.set`.

#### Scenario: no client quest.set route
- **GIVEN** the client quest book
- **THEN** the client SHALL NOT send quest progress writes via `meta_db.quest.set`
- **AND** the only client→server status-mutation path SHALL be `QuestCompleteRequest`, which is server-authoritative and specified in `manual-completion`
