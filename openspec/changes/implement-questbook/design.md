## Context

Questbook is a cross-cutting feature spanning 5 services/modules:
1. `src/libs/quest_lib/` — Shared data model + DAG evaluator
2. `src/services/simulation_core/` — Completion detection (QuestManager)
3. `src/services/meta_db/` — Persistence (Go)
4. `src/services/gateway/` — Message relay (C++)
5. `src/services/game_client/` — UI (C++/ImGui)

Plus protocol (`src/protocol/quest.fbs`) and data (`data/quests/`).

Current state: ~90% of code exists but had 10 known gaps. Server-side + data gaps closed (SimCore detection wiring, MetaDB CSV defs, topic split, data alignment); remaining gaps are client-side (QuestBookWindow FB parsing, manual completion, era UI) and deferred features (TOOL_CHARGED/SIDE_CONFIGURED handlers, era transition, QuestGraph-in-detection refactor, reward→inventory).

## Goals / Non-Goals

**Goals:**
- Complete the questbook feature: all listed tasks in `tasks.md` reachable by a local agent
- Fix stubs: `LoadGraph()`, `GetQuestDefinition()`, `distributeRewards()`
- Wire QuestManager into SimulationCore's craft + block place pipeline
- Integrate QuestGraph for DAG evaluation (replace inline prereq checking)
- Add TOOL_CHARGED + SIDE_CONFIGURED detection handlers
- Add era transition detection via QuestGraph::IsEraComplete()
- Add manual completion button + unlock notification to client UI
- Switch client from raw binary to FlatBuffers deserialization for quest messages
- Wire reward redemption to inventory system

**Non-Goals:**
- Create new quest data content (existing 36 quests are sufficient)
- Design a new quest data format (CSV + JSON is adequate)
- Add Lua/Python scripting for quest conditions (deferred)
- Build a quest editor tool

## Decisions

### Decision 1: QuestManager wiring order
Wire QuestManager into SimulationCore via `CraftRequestHandler` first (craft detection is the primary detection path), then block place pipeline, then tool/side config.

**Why**: Craft detection covers 95% of quest completions (30 of 36 quests). Block placed covers 5%. Tool/side config is <1%.

**Implementation**: 
- `CraftRequestHandler.cpp` — call `QuestManager::checkCraftCompletion()` after successful craft
- `DrillSystem.cpp` — call `QuestManager::checkCraftCompletion()` (drills are crafted items) and `QuestManager::checkToolCharge()` 
- Block place event handler — call `QuestManager::checkBlockAction()`

### Decision 2: QuestGraph integration — replace inline prereq checking
QuestManager currently does inline prerequisite loops (QuestManager.cpp:62-71, 140-149). Replace with `QuestGraph::CanComplete()` + `QuestGraph::NewlyAvailable()`.

**Why**: Duplicated logic violates DRY. QuestGraph already has correct DAG traversal. Using QuestGraph ensures consistent unlock behavior across server and client.

**Implementation**: 
1. Fix `QuestData::LoadGraph()` to parse quest_graph.json into `graph_` map  
2. Add `QuestData::BuildPrereqMap()` → returns `unordered_map<questId, vector<uint32_t>>`
3. Add `QuestData::BuildQuestEraMap()` → returns `unordered_map<questId, Era>`
4. Init QuestGraph with these maps in QuestManager constructor
5. Replace inline checks with `QuestGraph::CanComplete()`
6. After each completion, call `QuestGraph::NewlyAvailable()` for unlock events

### Decision 3: Topic split — quest.completed vs quest.completed.notification  [IMPLEMENTED]
**D3 (final)**: `quest.completed` carries only the FlatBuffer `QuestCompleted` from SimulationCore → MetaDB (SimCore also publishes `quest.progress.updated` with `QuestProgressUpdate`). MetaDB persists the reward and re-publishes a FlatBuffer `QuestCompletedNotification` on the separate `quest.completed.notification` topic → Gateway → Client. This keeps the raw event stream clean and lets Gateway map `quest.completed.notification` to `kQuestCompletedNotification` (type 21).

**Why**: The original spec had MetaDB publishing the notification back on `quest.completed`, creating a loop hazard (SimCore and MetaDB both publishing on the same topic). The split makes the direction explicit.

**Data flow**: Client → Gateway → MessageRouter → MetaDB (`meta_db.quest.set`, raw 10+6n layout) → response `meta_db.quest.progress.update`. Progress pull: SimCore → `meta_db.quest.get` (raw `[player_id:8 LE]`) → MetaDB → `meta_db.quest.get.response` (raw `[player_id:8 LE][n:2 LE][(quest_id,status,progress)*n]`).

### Decision 4: GameClient uses QuestGraph locally
QuestBookWindow should load `QuestGraph` alongside `QuestData` for local unlock computation.

**Why**: The client needs to display quest lock/unlock state immediately without waiting for server sync. Server is authoritative but client computes tentative state for responsive UI.

**Implementation**: Load QuestGraph in `loadQuestData()`, use `QuestGraph::GetUnlocked()` for coloring + availability display. **Status: deferred** (client phase).

### Decision 5: QuestBookWindow parses FlatBuffers instead of raw binary
Refactor `updateQuestStatus()` (QuestBookWindow.cpp:200-215) to use `quest_generated.h` FlatBuffers parsing.

**Why**: Raw binary offset parsing is fragile, hardcodes field sizes, and diverges from the protocol schema. Using the generated FlatBuffers parser ensures type safety and schema consistency.

**Implementation**: 
1. Include `quest_generated.h` in `QuestBookWindow.cpp`
2. Replace manual `ptr[0] | (ptr[1] << 8) | ...` with `GetQuestProgressUpdate(data)->quests()`
3. Remove hardcoded msgType 19 in `OnNetworkUpdate()` — use `GatewayMsg::kQuestProgressUpdate` enum from generated code

### Decision 6: GetQuestDefinition loads from CSV in Go  [IMPLEMENTED]
Refactor `GetQuestDefinition()` in `reward_handlers.go` to parse `data/quests/quests.csv`.

**Why**: Hardcoded stub breaks quests 2-36. CSV is the single source of truth for quest data. Go service can read the same CSV the C++ services use.

**Implementation**: 
1. New `definitions.go`: `loadQuestDefinitions(csvPath)` — skips header, warns on malformed rows, builds `questDefs map[uint32]QuestDef` + `eraValues` map
2. `main.go` calls `loadQuestDefinitions("data/quests/quests.csv")` at startup (fallback to absolute path)
3. `GetQuestDefinition()` reads from the in-memory map (nil for unknown ids)

### Decision 7: Reward integration with inventory  [PARTIALLY IMPLEMENTED]
QuestManager::distributeRewards() no longer publishes a dead `quest.reward.distributed` topic — the reward item/count travel inside `QuestCompleted` (quest.completed). MetaDB stores them in `player_quest_rewards` and forwards the notification to the client.

**Why**: This is the established pattern — services publish event messages through the router rather than making direct RPC calls for non-critical operations. Reward → inventory insertion is deferred until the inventory system is ready (separate change).

## Architecture Diagram (Data Flow)

```
Client (Q key)
  ├── loadQuestData() → quests.csv + quest_graph.json
  │     → QuestData + QuestGraph (local unlock computation, deferred)
  │     → EraInfo[] with sections + quest IDs
  │
  ├── Craft item → CraftRequest → Gateway → SimCore
  │     → CraftRequestHandler::onCraftSuccess()
  │     → QuestManager::checkCraftCompletion()
  │     → prereq check via GetPrerequisites()
  │     → publish QuestCompleted (quest.completed)
  │     → publish QuestProgressUpdate (quest.progress.updated)
  │
  ├── Place block → PlayerAction → Gateway → SimCore
  │     → SetBlockCASHandler (RIGHT_MOUSE_CLICK)
  │     → QuestManager::checkBlockAction()
  │     → (same flow as craft)
  │
  ├── Player join → SimCore → MetaDB progress pull:
  │     → PlayerJoinedHandler → onPlayerJoined() + meta_db.quest.get (raw [player_id:8 LE])
  │     → MetaDB → meta_db.quest.get.response (raw 10+6n) → SimCore loadProgress()
  │
  └── Server → Gateway → Client (on any event):
        quest.progress.updated → OnNetworkUpdate(kQuestProgressUpdate)
        quest.unlocked → OnNetworkUpdate(kQuestUnlockNotification)   [topic wired, no publisher yet]
        quest.completed.notification → OnNetworkUpdate(kQuestCompletedNotification)
```

## Risks / Trade-offs

- **Client computes tentative unlock state** → Could diverge from server. Mitigation: Server is authoritative; client defers to server updates.
- **Binary protocol mismatch** → Client currently uses raw parsing instead of FlatBuffers. Mitigation: Refactor to generated code.
- **No tests** → No quest-specific tests exist. Mitigation: Tasks assume manual verification; tests deferred.
- **Reward inventory integration depends on inventory system** → Currently a stub. Mitigation: publish event topic; inventory system integration is a separate task.

## Migration Plan

1. Fix `LoadGraph()` stub (highest priority — blocks QuestGraph integration)
2. Add `BuildPrereqMap()` + `BuildQuestEraMap()` to QuestData
3. Wire QuestManager into CraftRequestHandler + block handler
4. Replace inline prereq checks with QuestGraph calls
5. Add TOOL_CHARGED + SIDE_CONFIGURED handlers
6. Wire era transition (IsEraComplete)
7. Fix GetQuestDefinition() in MetaDB
8. Refactor QuestBookWindow: FlatBuffers parsing + manual complete + unlock notifications
9. Wire reward distribution to inventory system

## Open Questions

- Should QuestManager be in a background thread or on the simulation tick? Currently uses mutex — thread-safe but may need integration with ECS tick.
- QuestGraph local computation on client: do we need server-authoritative unlock or is local computation sufficient? Trusted client model vs anti-cheat?
- Quest data editing: should quests.csv be editable at runtime or only at build time? Currently build-time only (loaded at startup).
