## 1. Data Layer — quest_lib

- [x] 1.1 Fix `QuestData::LoadGraph()` stub (`src/libs/quest_lib/QuestData.cpp:75`) — parse `quest_graph.json` (nlohmann/json), populate `graph_` map (parent_id → child_ids), build reverse prereq map
- [x] 1.2 Add `quest::QuestData::BuildPrereqMap()` — return `std::unordered_map<uint32_t, std::vector<uint32_t>>` of quest_id → prerequisites (implemented equivalently via existing `GetPrerequisites()` + `Graph()`; QuestGraph::Init wired in `simulation_core/main.cpp`)
- [ ] 1.3 Add `quest::QuestData::BuildQuestEraMap()` — return `std::unordered_map<uint32_t, quest::Era>` for quest_id → era lookup (needed by `QuestGraph::IsEraComplete()`) — **deferred** (era transition not wired, task 5.9)
- [x] 1.4 QuestTypes.h — enums (`QuestStatus`, `Era`, `DetectionType`) + structs (`QuestDef`, `QuestProgress`, `QuestProgressSnapshot`, `SectionInfo`, `EraInfo`) — `src/libs/quest_lib/QuestTypes.h`
- [x] 1.5 QuestData.h/.cpp — CSV parser, `BuildEraStructure()`, `GetQuest()`, `GetEraQuests()`, `GetSectionQuests()`, `GetPrerequisites()`, `GetChildren()`, `GetRootQuests()` — `src/libs/quest_lib/QuestData.h:10-39`, `QuestData.cpp:8-180`
- [x] 1.6 QuestGraph.h/.cpp — DAG evaluator: `Init()`, `CanComplete()`, `NewlyAvailable()`, `GetUnlocked()`, `IsEraComplete()` — `src/libs/quest_lib/QuestGraph.h:11-37`, `QuestGraph.cpp:5-66`
- [x] 1.7 CMakeLists.txt — Static lib `quest_lib` linked to flatbuffers, spdlog, nlohmann_json — `src/libs/quest_lib/CMakeLists.txt`
- [x] 1.8 `data/quests/quests.csv` — 37 quests across 4 eras with detection types + rewards
- [x] 1.9 `data/quests/quest_graph.json` — DAG edges for all 37 quests

## 2. Protocol

- [x] 2.1 `src/protocol/quest.fbs` — `QuestStatus` enum, `QuestProgressUpdate`, `QuestEntry`, `QuestUnlockNotification`, `QuestCompletedNotification`, `QuestCompleted` event, `QuestUnlocked` event
- [x] 2.2 `src/protocol/gateway.fbs:16-18` — GatewayMsg types 19 (QuestProgressUpdate), 20 (QuestUnlockNotification), 21 (QuestCompletedNotification)
- [x] 2.3 Generated Go stubs in `src/protocol/generated/go/Protocol/`

## 3. Storage — MetaDB (Go)

- [x] 3.1 `src/services/meta_db/db.go:36-64` — SQLite schema: `quest_progress` and `player_quest_rewards` tables
- [x] 3.2 `src/services/meta_db/quest_progress.go` — CRUD: `GetQuestProgress()`, `SetQuestProgress()`, `SetQuestProgressBatch()`, `GetQuestProgressCount()`, `DeleteQuestProgress()`
- [x] 3.3 `src/services/meta_db/quest_handlers.go` — `HandleQuestGet()` (topic `meta_db.quest.get` → raw response `meta_db.quest.get.response`), `HandleQuestSet()` (topic `meta_db.quest.set` → publishes `meta_db.quest.progress.update`), `HandleQuestCompleted()` (topic `quest.completed` → publishes `quest.completed.notification`)
- [x] 3.4 `src/services/meta_db/reward_handlers.go` — `StorePlayerQuestReward()`, `GetPlayerQuestRewards()`, `RedeemPlayerQuestReward()`, `BatchRedeemPlayerQuestRewards()`, `GetQuestRewardsStats()`
- [x] 3.5 Fix `GetQuestDefinition()` stub (`src/services/meta_db/reward_handlers.go:244`) — implement CSV-based quest definition lookup: new `definitions.go` with `loadQuestDefinitions(csvPath)` (header skip, malformed-row warnings, era map); `main.go` loads `data/quests/quests.csv` at startup
- [ ] 3.6 Wire reward redemption to inventory system — `RedeemPlayerQuestReward()` should call inventory API to add items, not just mark redeemed — **deferred** (inventory integration is a separate change)

## 4. Gateway (C++)

- [x] 4.1 Subscribe to quest topics — `src/services/gateway/main.cpp:128-130`: `quest.completed`, `quest.unlocked`, `quest.progress.updated`, `quest.completed.notification`
- [x] 4.2 Forward quest messages to client — `src/services/gateway/gateway.cpp:406-411`: maps topics to GatewayMsg types (incl. `quest.completed.notification` → `kQuestCompletedNotification`) and calls `send_to_client_ctrl_raw()`
- [ ] 4.3 Add quest message routing from client to MetaDB — forward `quest.get` and `quest.set` from client to router topics (currently handled by generic message forwarding) — **deferred** (client does not send quest messages yet)

## 5. SimulationCore — QuestManager (C++)

- [x] 5.1 `src/services/simulation_core/Quest/QuestManager.h` — Class with `onPlayerJoined()`, `checkCraftCompletion()`, `checkBlockAction()`, `loadProgress()`, `distributeRewards()`
- [x] 5.2 `src/services/simulation_core/Quest/QuestManager.cpp` — Implementation (321 lines, full detection + reward logic)
- [x] 5.3 Wire QuestManager into SimulationCore startup — `main.cpp`: instantiate `QuestData` (LoadCSV + LoadGraph), `QuestGraph` (Init from `AllQuests()` prereqs), `QuestManager` (publish callback → `routerClient->PublishRaw`); set `msgDeps.questManager`; subscribe `meta_db.quest.get.response`
- [x] 5.4 Wire `checkCraftCompletion()` into `CraftRequestHandler` (`src/services/simulation_core/Crafting/CraftRequestHandler.cpp`) — call after successful craft (4th ctor param `std::shared_ptr<QuestManager>`)
- [x] 5.5 Wire `checkBlockAction()` into block place pipeline — `SetBlockCASHandler`: new `BlockPlacedCallback` param, invoked on RIGHT_MOUSE_CLICK after `onBlockChanged`
- [ ] 5.6 Replace inline prereq checking with QuestGraph — `QuestManager` keeps inline loop over `questData_->GetPrerequisites()` (lines 103, 181). QuestGraph is initialized in main.cpp but `CanComplete()`/`NewlyAvailable()` not wired into detection — **deferred refactor** (functionally equivalent; no tests to guard the swap)
- [ ] 5.7 Implement TOOL_CHARGED detection handler — listen for tool charge events, call QuestManager — **deferred**
- [ ] 5.8 Implement SIDE_CONFIGURED detection handler — listen for wrench/side config events, call QuestManager — **deferred**
- [ ] 5.9 Wire era transition — after completing a quest, call `QuestGraph::IsEraComplete()`, publish era transition event if full era done — **deferred** (requires BuildQuestEraMap, task 1.3)
- [ ] 5.10 Publish `QuestUnlocked` events — after each completion, call `QuestGraph::NewlyAvailable()` and publish results on `quest.unlocked` topic — **deferred** (status updates flow via `quest.progress.updated`; client handles those)
- [x] 5.11 Refactor `loadProgress()` to use FlatBuffers deserialization instead of raw binary offset parsing (lines 200-247) — **resolved differently**: MetaDB wire contract is raw binary `[player_id:8 LE][n:2 LE][(quest_id:4,status:1,progress:1)*n]` on `meta_db.quest.get.response` (not FlatBuffers). `loadProgress()` parses this exact format with size/status validation — no FlatBuffers involvement needed

## 6. Client UI — QuestBookWindow (C++)

- [x] 6.1 `src/services/game_client/UI/Windows/player/QuestBookWindow.h` — Class with era/section/quest data structures, layout helpers
- [x] 6.2 `src/services/game_client/UI/Windows/player/QuestBookWindow.cpp` — ImGui window (234 lines): 3-panel layout, era tabs, section list, quest list, detail view, status badges, network update handler, Q key toggle
- [ ] 6.3 Refactor `updateQuestStatus()` (`QuestBookWindow.cpp:200-215`) to use FlatBuffers deserialization instead of raw binary parsing — **deferred** (client phase)
- [ ] 6.4 Refactor `OnNetworkUpdate()` (`QuestBookWindow.cpp:217-224`) to use GatewayMsg enum constants instead of hardcoded `19` — **deferred** (client phase)
- [ ] 6.5 Add manual completion button — render in detail view when quest status is AVAILABLE; send `QuestProgressUpdate` on click — **deferred** (client phase)
- [ ] 6.6 Add unlock animation — when new quests become available, show brief visual effect (color pulse, icon change) — **deferred** (client phase)
- [ ] 6.7 Add completion indicator — badges on era tabs showing completion ratio (e.g., "3/12") — **deferred** (client phase)
- [ ] 6.8 Add era lock/unlock visual state — locked era tabs should be dimmed/gray until unlocked — **deferred** (client phase)

## 7. Quest Data Content

- [x] 7.1 37 quest definitions in `data/quests/quests.csv`
- [x] 7.2 DAG graph in `data/quests/quest_graph.json`
- [x] 7.3 Add more quests to fill gaps — data reworked: duplicate quest 36 removed, block_placed quests 37/38/39 added (targets 36/14/37, prereqs [5]/[4]/[6]); python validation passes (38 rows csv↔json 1:1, all prereqs exist)
- [x] 7.4 Align detect_target values with actual `item_id`/`block_id` values from items.csv registry — **resolved**: detect_target = flat recipe id (36=furnace, 14=workbench, 37=chest), NOT machine block_id; QuestManager compares `std::to_string(itemId/blockId)` with detectTarget; machines with hierarchical ids (`1110:xx:y`) excluded from craft detection

## Files Summary (implemented)

```
src/protocol/quest.fbs                          — 72 lines
src/protocol/gateway.fbs                        — types 19, 20, 21
src/libs/quest_lib/QuestTypes.h                 — 104 lines
src/libs/quest_lib/QuestTypes.cpp               — 2 lines
src/libs/quest_lib/QuestData.h                  — 41 lines
src/libs/quest_lib/QuestData.cpp                — 180 lines
src/libs/quest_lib/QuestGraph.h                 — 39 lines
src/libs/quest_lib/QuestGraph.cpp               — 66 lines
src/libs/quest_lib/CMakeLists.txt               — 20 lines
data/quests/quests.csv                          — 37 lines (36 quests)
data/quests/quest_graph.json                    — 40 lines (36 nodes)
src/services/meta_db/quest_progress.go          — 105 lines
src/services/meta_db/quest_handlers.go          — 212 lines
src/services/meta_db/reward_handlers.go         — 336 lines
src/services/gateway/gateway.cpp:406-411        — topic forwarding
src/services/gateway/main.cpp:128-130           — topic subscriptions
src/services/simulation_core/Quest/QuestManager.h       — 48 lines
src/services/simulation_core/Quest/QuestManager.cpp     — 299 lines
src/services/game_client/UI/Windows/player/QuestBookWindow.h  — 70 lines
src/services/game_client/UI/Windows/player/QuestBookWindow.cpp — 234 lines
```
