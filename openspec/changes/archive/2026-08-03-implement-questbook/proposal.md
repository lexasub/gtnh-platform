# Change: Implement Quest Book

## Why
Quest Book is the player's progression guide — equivalent of GTNH quest book. Shows what to craft, what to build, and unlocks next steps automatically. Provides structured progression through 4 eras (Vagrant → Apprentice → Expert → Administrator) with auto-detection of key milestones.

Currently: server-side wiring is done (QuestManager in SimulationCore, MetaDB CSV definitions + topic split, Gateway forwarding). Remaining gaps are client-side (FlatBuffers parsing, manual completion, notifications) and deferred features (TOOL_CHARGED/SIDE_CONFIGURED handlers, era transitions, QuestGraph-in-detection).

## What Changes

### Data Layer
- `data/quests/quests.csv` — 38 quests across 4 eras, sections, detection types, rewards (exists; duplicate quest 36 removed, block_placed quests 37/38/39 added)
- `data/quests/quest_graph.json` — DAG edges with visual position hints (exists)
- `src/libs/quest_lib/` — Shared static library: `QuestTypes.h` (enums + structs), `QuestData.h/.cpp` (CSV parser, `BuildEraStructure()`, `LoadGraph()` parses quest_graph.json + validates against CSV), `QuestGraph.h/.cpp` (DAG evaluator: `NewlyAvailable()`, `CanComplete()`, `IsEraComplete()`, `GetUnlocked()`)
  - **DONE**: `LoadGraph()` parses quest_graph.json (validation against CSV)
  - **PARTIAL**: `QuestGraph` is instantiated + Init'd in simcored main, but detection still uses inline `GetPrerequisites()` (task 5.6 deferred)

### Storage (MetaDB — Go)
- `src/services/meta_db/db.go:36-64` — SQLite tables: `quest_progress` (player_id, quest_id, status, progress_percent[reserved]) + `player_quest_rewards` (id, player_id, quest_id, reward_type, reward_id, reward_count, redeemed, metadata)
- `src/services/meta_db/quest_progress.go` — CRUD: `GetQuestProgress()`, `SetQuestProgress()`, `SetQuestProgressBatch()`, `GetQuestProgressCount()`, `DeleteQuestProgress()`
- `src/services/meta_db/quest_handlers.go` — `HandleQuestCompleted()` (topic `quest.completed` → publishes `QuestCompletedNotification` on `quest.completed.notification`). FlatBuffers `HandleQuestGet`/`HandleQuestSet` are **dead code** — routing goes through the raw-binary handlers in `router_client.go` (`meta_db.quest.get`/`meta_db.quest.set`)
- `src/services/meta_db/reward_handlers.go` — `StorePlayerQuestReward()`, `GetPlayerQuestRewards()`, `RedeemPlayerQuestReward()`, `BatchRedeemPlayerQuestRewards()`
  - **DONE**: `GetQuestDefinition()` reads from `questDefs` map loaded at startup from `quests.csv` via `definitions.go:loadQuestDefinitions()`

### Protocol
- `src/protocol/quest.fbs` — `QuestStatus` enum (LOCKED/AVAILABLE/IN_PROGRESS/COMPLETED), `QuestProgressUpdate`, `QuestEntry`, `QuestUnlockNotification`, `QuestCompletedNotification`, `QuestCompleted` event, `QuestUnlocked` event
- `src/protocol/gateway.fbs:16-18` — GatewayMsg types: 19=QuestProgressUpdate, 20=QuestUnlockNotification, 21=QuestCompletedNotification
- Generated Go stubs in `src/protocol/generated/go/Protocol/`

### Gateway (C++)
- `src/services/gateway/gateway.cpp:406-411` — Forwards quest topics to client: `quest.progress.updated` → msg 19, `quest.unlocked` → msg 20, `quest.completed.notification` → msg 21
- `src/services/gateway/main.cpp:128-130` — Subscribes to `quest.completed.notification`, `quest.unlocked`, `quest.progress.updated` (NOT `quest.completed` — internal SimCore→MetaDB)

### SimulationCore (C++) — QuestManager
- `src/services/simulation_core/Quest/QuestManager.h/.cpp` — Tracks per-player progress in-memory (`std::unordered_map<playerId, questId→status>`)
  - `onPlayerJoined()` — Seeds all quests LOCKED (graph invariant) + `loadProgress()` pulls from MetaDB
  - `checkCraftCompletion()` — Scans all quests with `DetectionType::CRAFT`, checks prereqs inline, marks COMPLETED, triggers `distributeRewards()`
  - `checkBlockAction()` — Same for `DetectionType::BLOCK_PLACED`
  - `loadProgress()` — Parses MetaDB raw `[player_id:8 LE][n:2 LE][(quest_id:4,status:1,progress:1)*n]` wire format with size/status validation
  - `distributeRewards()` — Logs reward + publishes `QuestCompleted` event (no reward fields; MetaDB resolves reward from CSV)
  - **DONE**: Wired into `simcored main` (instantiate + register publish callback + subscribe `meta_db.quest.get.response`), `CraftRequestHandler` (post-craft), `SetBlockCASHandler` (block place), `PlayerJoinedHandler` (join → progress pull)
  - **DEFERRED**: TOOL_CHARGED / SIDE_CONFIGURED detection handlers (no such quests in data)
  - **DEFERRED**: `checkCraftCompletion()`/`checkBlockAction()` still inline prereq checking — QuestGraph `CanComplete()`/`NewlyAvailable()` not wired (task 5.6)

### Client UI
- `src/services/game_client/UI/Windows/player/QuestBookWindow.h/.cpp` — ImGui window (Q key)
  - 3-panel layout: section list (left), quest list (middle), detail view (right)
  - Era tabs: Vagrant, Apprentice, Expert, Administrator
  - `renderEraTabs()`, `renderSectionPanel()`, `renderQuestList()`, `renderQuestDetail()`
  - `renderCompletionBadge()` — Status-dependent coloring (LOCKED gray, AVAILABLE yellow, COMPLETED green)
  - `updateQuestStatus()` — Raw 6-byte parsing (not FlatBuffers; task 6.3 deferred)
  - `OnNetworkUpdate()` — Handles msgType 19 only (hardcoded, 1024-byte copy; tasks 6.3/6.4 deferred)
  - **GAP**: No manual completion button (deferred — needs client→server quest message type)
  - **GAP**: No unlock/completion notification handling for msgTypes 20/21 (deferred)
  - **GAP**: Raw binary parsing instead of FlatBuffers deserialization (deferred)

### MessageRouter Pub/Sub Topics
| Topic | Direction | Payload |
|-------|-----------|---------|
| `meta_db.quest.get` | SimCore→MetaDB | Raw `[player_id:8 LE]` |
| `meta_db.quest.get.response` | MetaDB→SimCore | Raw `[player_id:8 LE][n:2 LE][(quest_id:4,status:1,progress:1)*n]` |
| `meta_db.quest.set` | Client→MetaDB (unused) | Raw entry layout |
| `meta_db.quest.progress.update` | MetaDB→SimCore | Raw per-entry event |
| `quest.progress.updated` | QuestManager/MetaDB→Gateway→Client | `QuestProgressUpdate` |
| `quest.completed` | SimCore→MetaDB (internal) | `QuestCompleted` (player_id, quest_id, timestamp) |
| `quest.completed.notification` | MetaDB→Gateway→Client | `QuestCompletedNotification` |
| `quest.unlocked` | (wired, no publisher yet) | `QuestUnlocked` / `QuestUnlockNotification` |

## Impact
- Affected specs: questbook (new)
- Affected code:
  - `src/libs/quest_lib/` — DONE: `LoadGraph()` parses graph; QuestGraph instantiated + Init'd in simcored main. REMAINING: wire QuestGraph into detection (task 5.6, deferred)
  - `src/services/simulation_core/` — DONE: QuestManager wired into CraftRequestHandler + SetBlockCASHandler + PlayerJoinedHandler. REMAINING: TOOL_CHARGED/SIDE_CONFIGURED handlers + era transitions (deferred)
  - `src/services/meta_db/` — DONE: `GetQuestDefinition()` from CSV (`definitions.go`). REMAINING: reward redemption → inventory (deferred, separate change)
  - `src/services/gateway/` — DONE: subscribes `quest.completed.notification`/`quest.unlocked`/`quest.progress.updated`
  - `src/services/game_client/` — REMAINING: manual complete button, FlatBuffers parsing, msgTypes 20/21 handling, completion animation (all deferred)
- New topics: `meta_db.quest.get`, `meta_db.quest.get.response`, `meta_db.quest.set`, `meta_db.quest.progress.update`, `quest.progress.updated`, `quest.completed`, `quest.completed.notification`, `quest.unlocked` (wired, no publisher yet)
