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

## MODIFIED Requirements

### Requirement: Quest Data Model
The system SHALL support a quest progression system with eras, sections, and individual quests.

#### Scenario: Quests are organized in eras
- **GIVEN** the quest data
- **THEN** quests are grouped into eras: Vagrant, Apprentice, Expert, Administrator
- **AND** eras contain sections (Foundation, Electric Tools, Machine Config, Transport, Market...)
- **AND** sections contain individual quests

#### Scenario: Quest data loaded from CSV + JSON
- **GIVEN** the system starts
- **WHEN** the quest library loads data
- **THEN** it SHALL parse `data/quests/quests.csv` for quest definitions (id, title, description, era, section, prereqs, detect_type, detect_target, reward_item, reward_count, cost_item, cost_count, cooldown)
- **AND** columns `cost_item`, `cost_count`, `cooldown` SHALL default to 0 when empty/missing (backward compatible with 10-column rows)
- **AND** it SHALL parse `data/quests/quest_graph.json` for DAG edges (prerequisites, position hints)
- **AND** it SHALL fail gracefully with a log warning if either file is missing or malformed

#### Scenario: Quest definition struct
- **GIVEN** a quest is defined
- **THEN** its definition SHALL include: unique id (uint32), title, description, era (enum: VAGRANT/APPRENTICE/EXPERT/ADMINISTRATOR), section name, prerequisites list, detection type (enum: CRAFT/BLOCK_PLACED/TOOL_CHARGED/SIDE_CONFIGURED/EXCHANGE), detection target string, reward item id (uint16), reward count (uint8), cost item id (uint16, default 0), cost count (uint8, default 0), cooldown seconds (uint16, default 0)

#### Scenario: BuildEraStructure produces UI-ready hierarchy
- **GIVEN** quest data is loaded
- **WHEN** `QuestData::BuildEraStructure()` is called
- **THEN** it SHALL return a vector of `EraInfo`, each containing sections with their quest IDs
- **AND** the ordering SHALL be: Vagrant → Apprentice → Expert → Administrator

### Requirement: FlatBuffers Quest Protocol
The system SHALL define FlatBuffers schema for quest-related messages.

#### Scenario: Client-Gateway quest message types
- **GIVEN** the protocol schema
- **THEN** `GatewayMsg::kQuestProgressUpdate` (type 20) SHALL carry `QuestProgressUpdate` FlatBuffer to client
- **AND** `GatewayMsg::kQuestUnlockNotification` (type 21) SHALL carry `QuestUnlockNotification` FlatBuffer
- **AND** `GatewayMsg::kQuestCompletedNotification` (type 22) SHALL carry `QuestCompletedNotification` FlatBuffer

#### Scenario: Quest status FlatBuffers enum
- **GIVEN** the protocol schema
- **THEN** `QuestStatus` SHALL be: LOCKED=0, AVAILABLE=1, IN_PROGRESS=2, COMPLETED=3
- **AND** it SHALL match `quest::QuestStatus` in `QuestTypes.h` for cross-service consistency
- **AND** the operative statuses SHALL be LOCKED/AVAILABLE/COMPLETED — IN_PROGRESS=2 and `progress` fields are reserved in the schema but no producer emits them
- **AND** exchange quests SHALL remain AVAILABLE=1 indefinitely (never COMPLETED)

#### Scenario: Quest events for pub/sub
- **GIVEN** a quest event occurs
- **THEN** `QuestCompleted` SHALL carry player_id, quest_id, timestamp (unix nanos)
- **AND** `QuestUnlocked` SHALL carry player_id, list of unlocked quest IDs (uint32[])

#### Scenario: Exchange message types
- **GIVEN** the protocol schema
- **THEN** `GatewayMsg::kQuestExchangeRequest` (26) SHALL carry `QuestExchangeRequest` (quest_id: uint32) from client
- **AND** `GatewayMsg::kQuestExchangeResponse` (27) SHALL carry `QuestExchangeResponse` (quest_id: uint32, success: bool, error_message: string, cooldown_remaining_secs: uint32) to client
- **AND** `GatewayMsg::kQuestExchangeCooldownGet` (28) SHALL carry `QuestExchangeCooldownGet` (quest_id: uint32) from client
- **AND** `GatewayMsg::kQuestExchangeCooldown` (29) SHALL carry `QuestExchangeCooldown` (quest_id: uint32, cooldown_remaining_secs: uint32) to client

### Requirement: Quest Storage in MetaDB
The system SHALL persist quest progress per player in MetaDB SQLite.

#### Scenario: Quest progress table schema
- **GIVEN** MetaDB initializes
- **THEN** `quest_progress` table SHALL exist with: player_id (FK to players), quest_id, status (uint8), progress_percent (uint8, reserved — unused)
- **AND** primary key SHALL be (player_id, quest_id)

#### Scenario: Player quest rewards table schema
- **GIVEN** MetaDB initializes
- **THEN** `player_quest_rewards` table SHALL exist with: id (PK autoincrement), player_id (FK), quest_id, reward_type, reward_id, reward_count, redeemed (bool), reward_timestamp, metadata
- **AND** it SHALL support filtering by redemption status (redeemed=0/1)
- **AND** each exchange trade SHALL insert a new reward row (repeatable)

#### Scenario: Exchange cooldown table schema
- **GIVEN** MetaDB initializes
- **THEN** `quest_exchange_cooldowns` table SHALL exist with: player_id (FK to players), quest_id, expires_at
- **AND** primary key SHALL be (player_id, quest_id)

#### Scenario: Quest progress CRUD operations
- **GIVEN** a player interacts with quests
- **WHEN** quest progress is requested
- **THEN** `GetQuestProgress()` SHALL return all quest_progress rows for the player
- **AND** `SetQuestProgress()` SHALL upsert a single entry
- **AND** `SetQuestProgressBatch()` SHALL upsert multiple entries in a transaction
- **AND** the wired raw-binary handlers in `router_client.go` SHALL serve `meta_db.quest.get` (raw `[player_id:8 LE]` request) → `meta_db.quest.get.response` (raw `[player_id:8 LE][n:2 LE][(quest_id:4 LE, status:1, progress:1)*n]`)
- **AND** `meta_db.quest.set` (same entry layout) SHALL update DB and publish `meta_db.quest.progress.update` per entry
- **AND** the FlatBuffers `HandleQuestGet`/`HandleQuestSet` in `quest_handlers.go` SHALL NOT be used for routing (dead code — router_client.go routes raw binary)

#### Scenario: Quest completed event persisted
- **GIVEN** an external event (`quest.completed` from SimulationCore carrying FlatBuffer `QuestCompleted` — player_id, quest_id, timestamp only)
- **WHEN** `HandleQuestCompleted` processes it
- **THEN** it SHALL resolve the reward via `GetQuestDefinition()` (from `quests.csv`) and store it in `player_quest_rewards`
- **AND** it SHALL publish FlatBuffer `QuestCompletedNotification` to `quest.completed.notification` topic for Gateway to forward to the Client
- **AND** `quest.completed` is SimCore→MetaDB internal only — MetaDB SHALL NOT re-publish on `quest.completed`

#### Scenario: Quest definition lookup from CSV
- **GIVEN** a quest completion event
- **WHEN** rewards need to be determined
- **THEN** `GetQuestDefinition()` SHALL return the quest definition loaded at startup from `data/quests/quests.csv` (`loadQuestDefinitions()` in `definitions.go`)
- **AND** return the reward_item_id and reward_count for that quest
- **AND** return cost_item_id, cost_count, cooldown for exchange quests

#### Scenario: Exchange processing in MetaDB
- **GIVEN** a player requests an exchange (topic `quest.exchange.request`)
- **WHEN** MetaDB processes it
- **THEN** it SHALL validate the quest definition (exists, `detectType == EXCHANGE`)
- **AND** SHALL check `quest_exchange_cooldowns` for an unexpired entry
- **AND** SHALL verify the player has `cost_item × cost_count` in inventory
- **AND** SHALL deduct cost items, insert cooldown entry (`expires_at = now + cooldown`), and grant the reward via `StorePlayerQuestReward` in a single SQLite transaction
- **AND** SHALL publish `quest.exchange.response` with success flag, error message (if any), and remaining cooldown seconds

### Requirement: Quest Completion Detection
The system SHALL automatically detect quest completion for supported detection types.

#### Scenario: Craft completion detected
- **GIVEN** a quest requires crafting a specific item (`DetectionType::CRAFT`, detectTarget = item_id)
- **WHEN** the player crafts that item (detected by CraftRequestHandler or inventory system)
- **THEN** `QuestManager::checkCraftCompletion()` SHALL check all craft-type quests
- **AND** SHALL mark the quest COMPLETED if both: prerequisites are met AND the crafted item matches detectTarget
- **AND** SHALL make the quest AVAILABLE if prerequisites just became satisfied
- **AND** SHALL publish `QuestCompleted` event on `quest.completed` topic
- **AND** SHALL distribute rewards via `distributeRewards()`

#### Scenario: Block placed completion detected
- **GIVEN** a quest requires placing a specific block (`DetectionType::BLOCK_PLACED`, detectTarget = block_id)
- **WHEN** the player places that block
- **THEN** `QuestManager::checkBlockAction()` SHALL handle completion identically to craft detection

#### Scenario: Exchange quests excluded from auto-detection
- **GIVEN** a quest with `DetectionType::EXCHANGE`
- **THEN** no passive detection path (`checkCraftCompletion`, `checkBlockAction`) SHALL complete it
- **AND** `QuestManager::completeQuest()` SHALL reject it (log + return false)
- **AND** completion SHALL happen only through the MetaDB exchange handler

### Requirement: Client Quest Book UI
The system SHALL provide a quest book UI window.

#### Scenario: Window opened with Q key
- **GIVEN** the client is running
- **WHEN** the player presses Q
- **THEN** the Quest Book window SHALL open
- **AND** SHALL toggle closed on second press

#### Scenario: Three-panel layout
- **GIVEN** the Quest Book window is open
- **THEN** it SHALL show era tabs at the top
- **AND** section list in the left panel
- **AND** quest list in the middle panel
- **AND** quest detail view in the right panel

#### Scenario: Quest detail shows description, status, rewards
- **GIVEN** a quest is selected in the middle panel
- **THEN** the right panel SHALL show: quest title, description, current status (colored badge), and reward info
- **AND** no progress bar — progress tracking is reserved/unused (all quests are single-shot)

#### Scenario: Exchange quest detail shows cost, reward, cooldown
- **GIVEN** a selected quest has `DetectionType::EXCHANGE`
- **THEN** the right panel SHALL show "Give: [cost_item_name] x[cost_count] → Receive: [reward_item_name] x[reward_count]"
- **AND** SHALL show the cooldown duration
- **AND** SHALL show an "Exchange" button
- **AND** the button SHALL be disabled with a countdown when server-reported cooldown > 0
- **AND** the button SHALL be disabled with a hint when the player lacks the cost items (client-side best-effort)
- **AND** when the client opens the detail view it SHALL send `QuestExchangeCooldownGet` (wire 28) to fetch the remaining cooldown

#### Scenario: Exchange response handled by UI
- **GIVEN** the client receives `QuestExchangeResponse` (wire 27)
- **WHEN** `success == true`
- **THEN** the client SHALL show a success toast and refresh the cooldown state
- **AND** when `success == false`
- **THEN** the client SHALL show an error toast with the error message (`unknown_quest`, `not_exchange`, `cooldown_active`, `missing_items`)

#### Scenario: Quest status reflected visually
- **GIVEN** quests are listed
- **THEN** LOCKED quests SHALL be gray
- **AND** AVAILABLE SHALL be yellow
- **AND** COMPLETED SHALL be green
- **AND** IN_PROGRESS is reserved/unused (no producer sets it)
- **AND** exchange quests SHALL always display AVAILABLE (yellow), never COMPLETED

#### Scenario: Quest progress synced from server
- **GIVEN** the client receives `QuestProgressUpdate` (msgType 20)
- **WHEN** `OnNetworkUpdate()` processes it
- **THEN** quest statuses SHALL be updated locally
- **AND** the UI SHALL reflect changes immediately

### Requirement: Reward Distribution
The system SHALL distribute quest rewards upon completion.

#### Scenario: Rewards logged and published
- **GIVEN** a quest is completed
- **WHEN** `QuestManager::distributeRewards()` runs
- **THEN** QuestManager SHALL log the reward and publish a `QuestCompleted` event (player_id, quest_id, timestamp) on `quest.completed` — the event carries no reward fields
- **AND** `MetaDB::HandleQuestCompleted` SHALL resolve the reward from `quests.csv` via `GetQuestDefinition()`, store it in `player_quest_rewards`, and forward a completion notification to the client

#### Scenario: Exchange rewards granted directly by MetaDB
- **GIVEN** an exchange quest is traded
- **WHEN** the MetaDB exchange handler succeeds
- **THEN** it SHALL store the reward in `player_quest_rewards` via `StorePlayerQuestReward` (a new row per trade)
- **AND** SHALL NOT publish `QuestCompleted` (exchange quests never complete — `quest.completed` remains SimCore-only)
