## MODIFIED Requirements

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
- **AND** it SHALL parse the trailing `target_count` column (INVENTORY objective quantity; 0 → treated as ≥1)
- **AND** it SHALL parse the EXCHANGE columns (`cost_item`, `cost_count`, `cooldown`)
- **AND** it SHALL parse `data/quests/quest_graph.json` for DAG edges (prerequisites, position hints)
- **AND** it SHALL fail gracefully with a log warning if either file is missing or malformed

#### Scenario: Quest definition struct
- **GIVEN** a quest is defined
- **THEN** its definition SHALL include: unique id (uint32), title, description, era (enum: VAGRANT/APPRENTICE/EXPERT/ADMINISTRATOR), section name, prerequisites list, detection type (enum: CRAFT/BLOCK_PLACED/TOOL_CHARGED/SIDE_CONFIGURED/INVENTORY), detection target string, reward item id (uint16), reward count (uint8)
- **AND** INVENTORY quests SHALL carry a `targetCount` (default 0 = ≥1)

#### Scenario: BuildEraStructure produces UI-ready hierarchy
- **GIVEN** quest data is loaded
- **WHEN** `QuestData::BuildEraStructure()` is called
- **THEN** it SHALL return a vector of `EraInfo`, each containing sections with their quest IDs
- **AND** the ordering SHALL be: Vagrant → Apprentice → Expert → Administrator

## ADDED Requirements

### Requirement: Inventory Detection
The system SHALL detect quest completion from the player's held inventory (`DetectionType::INVENTORY = 5`), evaluated server-side against the authoritative player inventory when the quest book is opened.

#### Scenario: Quest book open triggers inventory check
- **GIVEN** the player opens the quest book
- **WHEN** the client sends `QuestBookOpen` (`GatewayMsg::kQuestBookOpen = 33`) forwarded on the `quest.book.open` topic
- **THEN** SimulationCore SHALL snapshot `PlayerInventoryStore::getSlots(playerId)` into `QuestManager::checkInventory(playerId, slots)`
- **AND** it SHALL aggregate held quantity per hierarchical item id across all inventory slots

#### Scenario: INVENTORY quest completes when objective met
- **GIVEN** an INVENTORY quest with detectTarget and targetCount
- **WHEN** the held quantity of detectTarget across the player inventory meets `QuestGraph::CanComplete` prerequisites
- **AND** the objective (`targetCount`, 0 → ≥1) is satisfied
- **THEN** the quest SHALL complete via the one-step path (quest.completed + reward + era transition + unlock cascade)

#### Scenario: Objective shown in quest book UI
- **GIVEN** an INVENTORY quest is displayed in the quest book
- **THEN** the detail view SHALL show the objective ("Have X / N")
- **AND** it SHALL be colored green when the objective is met