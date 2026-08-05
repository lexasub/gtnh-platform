# manual-completion Specification

## Purpose
TBD - created by archiving change manual-completion. Update Purpose after archive.
## Requirements
### Requirement: Manual completion request
The client SHALL request quest completion via a dedicated server-authoritative message, never by setting status directly.

#### Scenario: Complete button sends request
- **GIVEN** a quest with status AVAILABLE is selected in the quest book detail view
- **WHEN** the player clicks "Complete"
- **THEN** the client SHALL send `QuestCompleteRequest` (not `quest.set`)

### Requirement: Server-authoritative validation
The server SHALL validate every completion request before changing status or granting rewards.

#### Scenario: Valid completion accepted
- **GIVEN** the server receives `QuestCompleteRequest` for a quest with status AVAILABLE
- **WHEN** `QuestManager::completeQuest()` validates prerequisites via `QuestGraph::CanComplete()`
- **THEN** the quest SHALL transition to COMPLETED
- **AND** `quest.completed` + `quest.progress.updated` SHALL be published
- **AND** newly available dependents SHALL be published on `quest.unlocked`
- **AND** the client SHALL receive `QuestCompletedNotification`

#### Scenario: Invalid completion rejected
- **GIVEN** the server receives `QuestCompleteRequest` for a LOCKED, already-COMPLETED, or unknown quest id
- **WHEN** `completeQuest()` validates it
- **THEN** the request SHALL be rejected
- **AND** the quest status SHALL NOT change and no rewards SHALL be granted

### Requirement: Reward delivery
Accepted completions SHALL grant the quest reward to the player inventory.

#### Scenario: Reward granted to inventory
- **GIVEN** a quest completes with `rewardItemId` / `rewardCount` set
- **THEN** the reward items SHALL be added to the player inventory via `RedeemPlayerQuestReward()`
- **AND** a second completion of the same quest SHALL NOT double-grant

### Requirement: Client confirmation
The client SHALL update the local quest status only after server acceptance.

#### Scenario: Optimistic UI with server confirmation
- **GIVEN** the player clicks "Complete" on an AVAILABLE quest
- **WHEN** the server accepts
- **THEN** the local status SHALL update to COMPLETED
- **AND** if the server rejects, the local status SHALL remain unchanged and the button SHALL remain available

