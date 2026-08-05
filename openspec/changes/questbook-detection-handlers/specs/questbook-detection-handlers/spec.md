## ADDED Requirements

### Requirement: Advanced Quest Detection
The system SHALL detect quest completion for all supported detection types.

#### Scenario: Craft completion via QuestGraph
- **GIVEN** a quest requiring an item craft (`DetectionType::CRAFT`, detectTarget = item id)
- **WHEN** the player crafts the item and the quest's prerequisites are met (`QuestGraph::CanComplete()` returns true)
- **THEN** the quest SHALL be marked COMPLETED in a single step
- **AND** prerequisites SHALL be evaluated via `QuestGraph::CanComplete()` instead of an inline loop

#### Scenario: Tool charge completion via CHARGE_ITEM
- **GIVEN** a quest requires charging a tool (`DetectionType::TOOL_CHARGED`, detectTarget = tool_id)
- **WHEN** the player sends a `CHARGE_ITEM` tool action and the tool's stored energy equals its capacity
- **THEN** `ToolActionHandler` SHALL forward the tool id to `QuestManager::checkToolCharged()`
- **AND** the quest SHALL be marked COMPLETED when prerequisites are met

#### Scenario: Side config completion via wrench
- **GIVEN** a quest requires configuring a machine side (`DetectionType::SIDE_CONFIGURED`)
- **WHEN** the player successfully cycles a machine face with the wrench and `WrenchCycleResult.machine_id` is non-zero
- **THEN** QuestManager SHALL be notified with the machine id via `checkSideConfigured()`
- **AND** the quest SHALL be marked COMPLETED when prerequisites are met

#### Scenario: Hatch wrenching is not side-config detection
- **GIVEN** the player cycles a multiblock hatch face with the wrench
- **WHEN** the hatch has no machine (`WrenchCycleResult.machine_id == 0`)
- **THEN** `checkSideConfigured()` SHALL NOT be called

#### Scenario: One-step completion semantics
- **GIVEN** a quest whose detection event fires and whose prerequisites are satisfied (`QuestGraph::CanComplete()` returns true)
- **WHEN** the quest is LOCKED or AVAILABLE
- **THEN** the quest SHALL transition directly to COMPLETED in a single step
- **AND** SHALL publish `QuestCompleted` on the `quest.completed` topic
- **AND** SHALL evaluate era completion via `maybePublishEraTransition()`
- **AND** rewards SHALL flow through the existing `quest.completed` → MetaDB path
- **AND** SHALL publish newly available quests on the `quest.unlocked` topic

### Requirement: QuestGraph-Backed Unlock Logic
The system SHALL evaluate unlocks via QuestGraph and publish unlock events.

#### Scenario: Parent quests unlock children via QuestGraph
- **GIVEN** a quest with prerequisites
- **WHEN** all parent quests are completed
- **THEN** `QuestGraph::CanComplete()` SHALL return true
- **AND** `QuestGraph::NewlyAvailable()` SHALL include this quest in the next unlock batch
- **AND** a `QuestUnlocked` event SHALL be published on the `quest.unlocked` topic

#### Scenario: DAG evaluated from QuestGraph not inline
- **GIVEN** QuestManager checks completion
- **WHEN** prerequisites need evaluation
- **THEN** QuestManager SHALL evaluate prerequisites via `QuestGraph::CanComplete()` instead of inline loops

#### Scenario: Detection completions cascade unlocks
- **GIVEN** a quest completed via a detection path (craft, block placed, tool charged, or side configured)
- **THEN** `QuestGraph::NewlyAvailable()` SHALL be evaluated
- **AND** newly available dependents SHALL be published on the `quest.unlocked` topic

### Requirement: Quest Data for Detection Types
The system SHALL ship quest data for the new detection types.

#### Scenario: Tool charging quest data
- **GIVEN** the quest data files `data/quests/quests.csv` and `data/quests/quest_graph.json`
- **THEN** they SHALL contain a quest with `DetectionType::TOOL_CHARGED` targeting the ULV drill (`1111:00:0`)
- **AND** the quest SHALL specify a reward item and count
- **AND** `quest_graph.json` SHALL contain the quest node with a graph position

#### Scenario: Side config quest data
- **GIVEN** the quest data files `data/quests/quests.csv` and `data/quests/quest_graph.json`
- **THEN** they SHALL contain a quest with `DetectionType::SIDE_CONFIGURED` targeting a machine (e.g. heat furnace `1110:00:0`)
- **AND** the quest SHALL specify a reward item and count
- **AND** `quest_graph.json` SHALL contain the quest node with a graph position
