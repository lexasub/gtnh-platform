## ADDED Requirements

### Requirement: Quest Data Model
The system SHALL support a quest progression system with eras, sections, and individual quests.

#### Scenario: Quests are organized in eras
- **GIVEN** the quest data
- **THEN** quests are grouped into eras: Vagrant, Apprentice, Expert, Administrator
- **AND** eras contain sections (Foundation, Electric Tools, Machine Config, Transport...)
- **AND** sections contain individual quests

#### Scenario: Quest data loaded from CSV + JSON
- **GIVEN** the system starts
- **WHEN** the quest library loads data
- **THEN** it SHALL parse `data/quests/quests.csv` for quest definitions (id, title, description, era, section, prereqs, detect_type, detect_target, reward_item, reward_count)
- **AND** it SHALL parse `data/quests/quest_graph.json` for DAG edges (prerequisites, position hints)
- **AND** it SHALL fail gracefully with a log warning if either file is missing or malformed

#### Scenario: Quest definition struct
- **GIVEN** a quest is defined
- **THEN** its definition SHALL include: unique id (uint32), title, description, era (enum: VAGRANT/APPRENTICE/EXPERT/ADMINISTRATOR), section name, prerequisites list, detection type (enum: CRAFT/BLOCK_PLACED/TOOL_CHARGED/SIDE_CONFIGURED), detection target string, reward item id (uint16), reward count (uint8)

#### Scenario: BuildEraStructure produces UI-ready hierarchy
- **GIVEN** quest data is loaded
- **WHEN** `QuestData::BuildEraStructure()` is called
- **THEN** it SHALL return a vector of `EraInfo`, each containing sections with their quest IDs
- **AND** the ordering SHALL be: Vagrant → Apprentice → Expert → Administrator

### Requirement: FlatBuffers Quest Protocol
The system SHALL define FlatBuffers schema for quest-related messages.

#### Scenario: Client-Gateway quest message types
- **GIVEN** the protocol schema
- **THEN** `GatewayMsg::kQuestProgressUpdate` (type 19) SHALL carry `QuestProgressUpdate` FlatBuffer to client
- **AND** `GatewayMsg::kQuestUnlockNotification` (type 20) SHALL carry `QuestUnlockNotification` FlatBuffer
- **AND** `GatewayMsg::kQuestCompletedNotification` (type 21) SHALL carry `QuestCompletedNotification` FlatBuffer

#### Scenario: Quest status FlatBuffers enum
- **GIVEN** the protocol schema
- **THEN** `QuestStatus` SHALL be: LOCKED=0, AVAILABLE=1, IN_PROGRESS=2, COMPLETED=3
- **AND** it SHALL match `quest::QuestStatus` in `QuestTypes.h` for cross-service consistency

#### Scenario: Quest events for pub/sub
- **GIVEN** a quest event occurs
- **THEN** `QuestCompleted` SHALL carry player_id, quest_id, timestamp (unix nanos)
- **AND** `QuestUnlocked` SHALL carry player_id, list of unlocked quest IDs (uint32[])

### Requirement: Quest Storage in MetaDB
The system SHALL persist quest progress per player in MetaDB SQLite.

#### Scenario: Quest progress table schema
- **GIVEN** MetaDB initializes
- **THEN** `quest_progress` table SHALL exist with: player_id (FK to players), quest_id, status (uint8), progress_percent (uint8)
- **AND** primary key SHALL be (player_id, quest_id)

#### Scenario: Player quest rewards table schema
- **GIVEN** MetaDB initializes
- **THEN** `player_quest_rewards` table SHALL exist with: id (PK autoincrement), player_id (FK), quest_id, reward_type, reward_id, reward_count, redeemed (bool), reward_timestamp, metadata
- **AND** it SHALL support filtering by redemption status (redeemed=0/1)

#### Scenario: Quest progress CRUD operations
- **GIVEN** a player interacts with quests
- **WHEN** quest progress is requested
- **THEN** `GetQuestProgress()` SHALL return all quest_progress rows for the player
- **AND** `SetQuestProgress()` SHALL upsert a single entry
- **AND** `SetQuestProgressBatch()` SHALL upsert multiple entries in a transaction
- **AND** `HandleQuestGet` (topic `meta_db.quest.get`, raw `[player_id:8 LE]` request) SHALL respond on `meta_db.quest.get.response` with raw `[player_id:8 LE][n:2 LE][(quest_id:4 LE, status:1, progress:1)*n]`
- **AND** `HandleQuestSet` (topic `meta_db.quest.set`, same entry layout) SHALL update DB and publish `meta_db.quest.progress.update` per entry

#### Scenario: Quest completed event persisted
- **GIVEN** an external event (`quest.completed` from SimulationCore carrying FlatBuffer `QuestCompleted`)
- **WHEN** `HandleQuestCompleted` processes it
- **THEN** it SHALL store reward in `player_quest_rewards`
- **AND** it SHALL publish FlatBuffer `QuestCompletedNotification` to `quest.completed.notification` topic for Gateway to forward to the Client

#### Scenario: Quest definition lookup from CSV
- **GIVEN** a quest completion event
- **WHEN** rewards need to be determined
- **THEN** `GetQuestDefinition()` SHALL return the quest definition loaded at startup from `data/quests/quests.csv` (`loadQuestDefinitions()` in `definitions.go`)
- **AND** return the reward_item_id and reward_count for that quest

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

#### Scenario: Tool charge completion
- **GIVEN** a quest requires charging a tool (`DetectionType::TOOL_CHARGED`, detectTarget = tool_id)
- **WHEN** the player tool reaches full charge
- **THEN** the quest SHALL be marked COMPLETED

#### Scenario: Side config completion
- **GIVEN** a quest requires configuring a machine side (`DetectionType::SIDE_CONFIGURED`)
- **WHEN** the player sets a machine side to a specified mode
- **THEN** the quest SHALL be marked COMPLETED

#### Scenario: Manual completion allowed
- **GIVEN** a quest has its prerequisites met but no auto-detection triggered
- **WHEN** the player clicks "Complete" in the quest detail view
- **THEN** the quest SHALL be marked as manually completed
- **AND** rewards SHALL be distributed

### Requirement: DAG Unlock Logic
The system SHALL unlock quests based on a DAG of prerequisites, using QuestGraph evaluation.

#### Scenario: Parent quests unlock children
- **GIVEN** a quest with prerequisites
- **WHEN** all parent quests are completed
- **THEN** `QuestGraph::CanComplete()` SHALL return true
- **AND** `QuestGraph::NewlyAvailable()` SHALL include this quest in the next unlock batch
- **AND** a `QuestUnlocked` event SHALL be published on `quest.unlocked` topic
- **AND** the client SHALL display an unlock notification

#### Scenario: DAG evaluated from QuestGraph not inline
- **GIVEN** QuestManager checks completion
- **WHEN** prerequisites need evaluation
- **THEN** QuestManager SHALL evaluate prerequisites from quest data via `GetPrerequisites()` (QuestGraph is initialized at startup in main.cpp and kept for future `CanComplete()`/`NewlyAvailable()` unlock flow)
- **AND** prerequisite evaluation SHALL mark a quest AVAILABLE only when all prerequisites are COMPLETED

#### Scenario: QuestGraph loaded from quest_graph.json
- **GIVEN** the system starts
- **WHEN** `QuestData::LoadGraph()` is called
- **THEN** it SHALL parse `data/quests/quest_graph.json` and populate the children/prereq maps, validating quest ids against the CSV and warning on prereq mismatches
- **AND** `QuestGraph::Init()` SHALL be called with the parsed data

### Requirement: Era Progression
The system SHALL track era completion and transition between eras.

#### Scenario: Era complete when all quests completed
- **GIVEN** a player has quests in Era::VAGRANT
- **WHEN** all quests in that era reach COMPLETED status
- **THEN** `QuestGraph::IsEraComplete()` SHALL return true
- **AND** a notification SHALL be sent to the client (first quest in next era unlocked)

#### Scenario: Era transition triggers next era
- **GIVEN** Era::VAGRANT is complete
- **WHEN** any quest from Era::APPRENTICE becomes unlocked
- **THEN** the client SHALL show the new era tab as active/available
- **AND** the era tabs UI SHALL indicate locked vs unlocked eras

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
- **THEN** the right panel SHALL show: quest title, description, current status (colored badge), progress bar (for IN_PROGRESS), and reward info

#### Scenario: Quest status reflected visually
- **GIVEN** quests are listed
- **THEN** LOCKED quests SHALL be gray
- **AND** AVAILABLE SHALL be yellow
- **AND** IN_PROGRESS SHALL be blue
- **AND** COMPLETED SHALL be green

#### Scenario: Quest progress synced from server
- **GIVEN** the client receives `QuestProgressUpdate` (msgType 19)
- **WHEN** `OnNetworkUpdate()` processes it
- **THEN** quest statuses SHALL be updated locally
- **AND** the UI SHALL reflect changes immediately

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
- **THEN** a `QuestProgressUpdate` SHALL be sent to MetaDB via Gateway
- **AND** the local status SHALL update to COMPLETED

### Requirement: Reward Distribution
The system SHALL distribute quest rewards upon completion.

#### Scenario: Rewards logged and published
- **GIVEN** a quest is completed
- **WHEN** `QuestManager::distributeRewards()` runs
- **THEN** the reward item/count SHALL travel inside the `QuestCompleted` event on `quest.completed` topic
- **AND** `MetaDB::HandleQuestCompleted` SHALL store it in `player_quest_rewards` and forward a completion notification to the client
- **AND** QuestManager SHALL log the reward for debugging

#### Scenario: Rewards integrated with inventory
- **GIVEN** a quest is completed with a non-zero reward
- **WHEN** the reward event is processed
- **THEN** the reward items SHALL eventually be added to the player inventory via MetaDB inventory system (**deferred** — rewards are stored in `player_quest_rewards`; inventory insertion is a separate change)
