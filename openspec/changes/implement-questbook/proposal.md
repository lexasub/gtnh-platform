# Change: Implement Quest Book

## Why
Quest Book is the player's progression guide — equivalent of GTNH quest book. Shows what to craft, what to build, and unlocks next steps automatically. Provides structured progression through 4 eras (Vagrant → Apprentice → Expert → Administrator) with auto-detection of key milestones.

Currently: quest data files, shared library (quest_lib), FlatBuffers schema, MetaDB storage + handlers, QuestManager in SimulationCore, Gateway forwarding, and Client UI exist but have gaps in wiring, DAG evaluation, and era transitions.

## What Changes

### Data Layer
- `data/quests/quests.csv` — 36 quests across 4 eras, sections, detection types, rewards (exists)
- `data/quests/quest_graph.json` — DAG edges with visual position hints (exists)
- `src/libs/quest_lib/` — Shared static library: `QuestTypes.h` (enums + structs), `QuestData.h/.cpp` (CSV parser, `BuildEraStructure()`), `QuestGraph.h/.cpp` (DAG evaluator: `NewlyAvailable()`, `CanComplete()`, `IsEraComplete()`, `GetUnlocked()`)
  - **GAP**: `QuestData::LoadGraph()` is a stub — doesn't parse quest_graph.json (line 75)
  - **GAP**: `QuestGraph` is never instantiated/used by QuestManager

### Storage (MetaDB — Go)
- `src/services/meta_db/db.go:36-64` — SQLite tables: `quest_progress` (player_id, quest_id, status, progress_percent) + `player_quest_rewards` (id, player_id, quest_id, reward_type, reward_id, reward_count, redeemed, metadata)
- `src/services/meta_db/quest_progress.go` — CRUD: `GetQuestProgress()`, `SetQuestProgress()`, `SetQuestProgressBatch()`, `GetQuestProgressCount()`, `DeleteQuestProgress()`
- `src/services/meta_db/quest_handlers.go` — MessageRouter handlers: `HandleQuestGet()` (topic `quest.get`), `HandleQuestSet()` (topic `quest.set`), `HandleQuestCompleted()` (topic `quest.completed` → publishes `QuestCompletedNotification`)
- `src/services/meta_db/reward_handlers.go` — `StorePlayerQuestReward()`, `GetPlayerQuestRewards()`, `RedeemPlayerQuestReward()`, `BatchRedeemPlayerQuestRewards()`
  - **GAP**: `GetQuestDefinition()` (line 244) is a stub — returns hardcoded data for quest ID 1 only

### Protocol
- `src/protocol/quest.fbs` — `QuestStatus` enum (LOCKED/AVAILABLE/IN_PROGRESS/COMPLETED), `QuestProgressUpdate`, `QuestEntry`, `QuestUnlockNotification`, `QuestCompletedNotification`, `QuestCompleted` event, `QuestUnlocked` event
- `src/protocol/gateway.fbs:16-18` — GatewayMsg types: 19=QuestProgressUpdate, 20=QuestUnlockNotification, 21=QuestCompletedNotification
- Generated Go stubs in `src/protocol/generated/go/Protocol/`

### Gateway (C++)
- `src/services/gateway/gateway.cpp:406-411` — Forwards quest topics to client: `quest.progress.updated` → msg 19, `quest.unlocked` → msg 20, `quest.completed` → msg 21
- `src/services/gateway/main.cpp:128-130` — Subscribes to all 3 quest topics

### SimulationCore (C++) — QuestManager
- `src/services/simulation_core/Quest/QuestManager.h/.cpp` — Tracks per-player progress in-memory (`std::unordered_map<playerId, questId→status>`)
  - `onPlayerJoined()` — Initialises player state
  - `checkCraftCompletion()` — Scans all quests with `DetectionType::CRAFT`, checks prereqs inline, marks COMPLETED, triggers `distributeRewards()`
  - `checkBlockAction()` — Same for `DetectionType::BLOCK_PLACED`
  - `loadProgress()` — Binary blob parsing from MetaDB (raw offset-based, not FlatBuffers)
  - `distributeRewards()` — Publishes `quest.reward.distributed` topic (logs only, no real inventory integration)
  - **GAP**: Not wired into SimulationCore `MachineSystem` or `CraftRequestHandler`
  - **GAP**: TOOL_CHARGED and SIDE_CONFIGURED detection types exist in enum but have no handlers
  - **GAP**: `checkCraftCompletion()` does inline prereq checking instead of using `QuestGraph`

### Client UI
- `src/services/game_client/UI/Windows/player/QuestBookWindow.h/.cpp` — ImGui window (Q key)
  - 3-panel layout: section list (left), quest list (middle), detail view (right)
  - Era tabs: Vagrant, Apprentice, Expert, Administrator
  - `renderEraTabs()`, `renderSectionPanel()`, `renderQuestList()`, `renderQuestDetail()`
  - `renderCompletionBadge()` — Status-dependent coloring (LOCKED gray, AVAILABLE yellow, IN_PROGRESS blue, COMPLETED green)
  - `updateQuestStatus()` — Raw binary parsing (not FlatBuffers)
  - `OnNetworkUpdate()` — Handles msgType 19 (hardcoded)
  - **GAP**: No manual completion button
  - **GAP**: No unlock animation or transition effect
  - **GAP**: Raw binary parsing instead of FlatBuffers deserialization

### MessageRouter Pub/Sub Topics
| Topic | Direction | Payload |
|-------|-----------|---------|
| `quest.get` | Client→Gateway→MetaDB | `QuestProgressUpdate` |
| `quest.set` | Client→Gateway→MetaDB | `QuestProgressUpdate` |
| `quest.progress.updated` | MetaDB→Gateway→Client | `QuestProgressUpdate` |
| `quest.completed` | SimulationCore→MetaDB→Gateway→Client | `QuestCompleted` / `QuestCompletedNotification` |
| `quest.unlocked` | QuestManager→Gateway→Client | `QuestUnlocked` / `QuestUnlockNotification` |
| `quest.reward.distributed` | QuestManager→internal | Binary (reward payload) |

## Impact
- Affected specs: questbook (new)
- Affected code:
  - `src/libs/quest_lib/` — Fix `LoadGraph()` stub, wire `QuestGraph` into QuestManager
  - `src/services/simulation_core/` — Wire QuestManager into CraftRequestHandler + block place pipeline; add TOOL_CHARGED/SIDE_CONFIGURED detection; integrate QuestGraph for DAG + era transitions
  - `src/services/meta_db/` — Implement `GetQuestDefinition()` from CSV; wire reward redemption to inventory system
  - `src/services/gateway/` — No changes needed (already subscribed + forwarding)
  - `src/services/game_client/` — Add manual complete button; switch to FlatBuffers parsing; add completion animation
- New topics: `quest.get`, `quest.set`, `quest.progress.updated`, `quest.completed`, `quest.reward.distributed`
