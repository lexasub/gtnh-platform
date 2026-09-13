## ADDED Requirements

### Requirement: Exchange Quest Processing (repeatable market)
The system SHALL support repeatable exchange quests where the player actively trades items for a reward, with a cooldown, without the quest ever completing.

#### Scenario: Exchange quest never completes
- **GIVEN** a quest with `DetectionType::EXCHANGE`
- **THEN** the quest SHALL remain `AVAILABLE` after every exchange
- **AND** SHALL never transition to `COMPLETED`
- **AND** SHALL be excluded from `QuestData::BuildQuestEraMap()` so it can never block `QuestGraph::IsEraComplete()` for its era
- **AND** `QuestManager::completeQuest()` SHALL reject exchange quests (log + return false)

#### Scenario: Exchange triggered by explicit client request
- **GIVEN** a quest with `DetectionType::EXCHANGE` in the quest detail view
- **WHEN** the player clicks the "Exchange" button
- **THEN** the client SHALL send `QuestExchangeRequest` (quest_id) via Gateway (wire 26)
- **AND** the Gateway SHALL publish it to the `quest.exchange.request` topic
- **AND** MetaDB SHALL process the exchange end-to-end: validate quest def, check cooldown, verify and deduct cost items, store cooldown, grant reward — in a single SQLite transaction
- **AND** MetaDB SHALL publish `quest.exchange.response` which the Gateway forwards to the client as wire 27

#### Scenario: Exchange rejection cases
- **GIVEN** a player requests an exchange
- **WHEN** the quest is unknown → **THEN** error `unknown_quest`
- **WHEN** the quest is not `EXCHANGE` type → **THEN** error `not_exchange`
- **WHEN** a cooldown is still active → **THEN** error `cooldown_active` including remaining seconds
- **WHEN** the player lacks `costItem × costCount` → **THEN** error `missing_items`
- **AND** in every rejection case SHALL NOT deduct items, SHALL NOT grant a reward, and SHALL NOT store a cooldown
### Requirement: Exchange Cooldown Persistence
The system SHALL persist exchange cooldowns per player per quest in MetaDB, server-authoritative.

#### Scenario: Cooldown table and enforcement
- **GIVEN** MetaDB initializes
- **THEN** `quest_exchange_cooldowns` table SHALL exist with: player_id (FK), quest_id, expires_at
- **AND** primary key SHALL be (player_id, quest_id)
- **AND** `expires_at` SHALL be computed server-side as `now + cooldownSecs` (client clocks never trusted)
- **AND** an exchange attempt with an unexpired `expires_at` SHALL be rejected with `cooldown_active`

#### Scenario: Cooldown query for client
- **GIVEN** a player opens a quest detail view for an exchange quest
- **WHEN** the client sends `QuestExchangeCooldownGet` (wire 28) via Gateway
- **THEN** the Gateway SHALL publish it to the `quest.exchange.cooldown.get` topic
- **AND** MetaDB SHALL respond with remaining cooldown in seconds (0 = no cooldown) via `quest.exchange.cooldown.response`
- **AND** the Gateway SHALL forward the response to the client as wire 29
### Requirement: Exchange Quest Data
The system SHALL ship exchange quest data with at least one exchange quest.

#### Scenario: Crafting table exchange quest
- **GIVEN** the quest data files `data/quests/quests.csv` and `data/quests/quest_graph.json`
- **THEN** quest 4 SHALL exist with `detectType == exchange`, empty `detect_target`, `cost_item=0:10:00:0` (oak plank), `cost_count=4`, `cooldown=60`
- **AND** reward SHALL be item `0:10:11:1` (crafting table) x1
- **AND** it SHALL be in `vagrant → market` section
- **AND** `quest_graph.json` SHALL contain the quest node with `prereqs=[]` in the `market` column
- **AND** quests 5 and 38 SHALL remain root quests (no dependency on quest 4, since exchange quests never complete)
