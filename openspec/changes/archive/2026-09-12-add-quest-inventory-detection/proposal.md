# Change: Questbook Inventory Detection

## Why
The quest book is a passive viewer: quests complete only through craft/block/tool/side-config actions. But GTNH-style progression is heavily "have N of X" gated — the player should get credit when they *own* the required items, not just when they perform a single action. There is no server path that re-evaluates "do you have the item?" and no client signal that the player is reviewing their quests. This change adds a new `DetectionType::INVENTORY` whose objective ("hold ≥ targetCount of detectTarget item") is checked server-side against the authoritative player inventory, triggered when the player opens the quest book.

## What Changes
- New `DetectionType::INVENTORY` in `QuestTypes.h`; new `QuestDef::targetCount` field parsed from a new trailing `target_count` column in `quests.csv` (0 → treated as ≥1).
- `QuestManager::checkInventory(playerId, slots)` — aggregates held quantity per hierarchical item id across all inventory slots, and for each INVENTORY quest whose objective is met and whose prerequisites are met (`QuestGraph::CanComplete`), completes it via the existing one-step path (quest.completed + reward + era transition + unlock cascade).
- New wire message `QuestBookOpen` (quest.fbs) + `GatewayMsg::kQuestBookOpen = 33` (gateway.h / NetClient.h / gateway.fbs union). Client sends it from `ActionHandler::DoToggleQuestBook` on open; gateway forwards to `quest.book.open`; SimulationCore handles it in `SimCoreMessageHandler` by snapshotting `PlayerInventoryStore::getSlots(playerId)` into `QuestManager::checkInventory`.
- Quest book UI shows the objective ("Have X / N") for INVENTORY quests, colored green when met.
- Quest data: two INVENTORY quests — id 42 (copper ore `10:3`, prereq 7, target 8), id 43 (iron ore `10:0`, prereq 18, target 16) in `quests.csv` + `quest_graph.json`.

## Impact
- Affected specs: questbook (modified — new detection type + quest data)
- Affected code:
  - `src/libs/quest_lib/QuestTypes.{h}` — `DetectionType::INVENTORY`, `QuestDef::targetCount`
  - `src/libs/quest_lib/QuestData.cpp` — parse `target_count` column
  - `src/services/simulation_core/Quest/QuestManager.{h,cpp}` — `checkInventory()`
  - `src/services/simulation_core/Network/SimCoreMessageHandler.cpp` — handle `quest.book.open`
  - `src/services/gateway/gateway.{h,cpp}` — `kQuestBookOpen = 33`, forward to `quest.book.open`
  - `src/services/game_client/Network/NetClient.{h,cpp}` — `SendQuestBookOpen()`
  - `src/services/game_client/UI/Core/ActionHandler.cpp` — notify server on quest book open
  - `src/services/game_client/UI/Windows/player/QuestBookWindow.{h,cpp}` — objective display
  - `src/protocol/quest.fbs`, `src/protocol/gateway.fbs` — `QuestBookOpen` message + union member
  - `data/quests/quests.csv`, `data/quests/quest_graph.json` — two INVENTORY quests
- No change to MetaDB: INVENTORY completions flow through the existing `quest.completed` path (reward grant + notification).
