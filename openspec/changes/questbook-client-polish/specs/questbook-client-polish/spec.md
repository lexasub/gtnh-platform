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
Locked eras SHALL be visually distinct and SHALL NOT be selectable.

#### Scenario: Locked era dimmed
- **GIVEN** the quest book is open
- **WHEN** an era's preceding era is not fully COMPLETED
- **THEN** the era tab SHALL be dimmed/gray and SHALL NOT be selectable
- **AND** when an era becomes unlocked it SHALL show a brief visual effect

### Requirement: Client quest progress routing
The client SHALL be able to request and submit quest progress through the gateway.

#### Scenario: quest.get round-trip
- **GIVEN** the client requests quest progress
- **WHEN** `NetClient::SendQuestGet` is called
- **THEN** the gateway SHALL forward the payload to `meta_db.quest.get`
- **AND** the response SHALL reach the client as `QuestProgressUpdate` (msgType 20)

#### Scenario: quest.set forwarding
- **GIVEN** the client submits quest progress updates
- **WHEN** `NetClient::SendQuestSet` is called
- **THEN** the gateway SHALL forward the payload to `meta_db.quest.set`
- **AND** this transport SHALL NOT be used to authorize status transitions — manual completion is server-authoritative in `manual-completion`
