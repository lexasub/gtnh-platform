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
- Create new quest data content (existing 38 quests are sufficient)
- Design a new quest data format (CSV + JSON is adequate)
- Add Lua/Python scripting for quest conditions (deferred)
- Build a quest editor tool

## Decisions

### Decision 1: QuestManager wiring order  [IMPLEMENTED]
Wire QuestManager into SimulationCore via `CraftRequestHandler` first (craft detection is the primary detection path), then block place pipeline, then tool/side config.

**Why**: Craft detection covers 92% of quest completions (35 of 38 quests). Block placed covers 3 quests (37/38/39). Tool/side config: 0 quests in data.

**Implementation**: 
- `CraftRequestHandler.cpp` — calls `QuestManager::checkCraftCompletion()` after successful craft ✔
- `SetBlockCASHandler.cpp` — calls `QuestManager::checkBlockAction()` on RIGHT_MOUSE_CLICK after `onBlockChanged` ✔
- `PlayerJoinedHandler.cpp` — calls `onPlayerJoined()` + progress pull via `meta_db.quest.get.response` ✔
- `DrillSystem.cpp`/tool-charge — **deferred** (no TOOL_CHARGED quests)

### Decision 2: QuestGraph integration — replace inline prereq checking  [PARTIAL]
QuestManager does inline prerequisite loops (QuestManager.cpp:103, 181). QuestGraph is instantiated + Init'd in `simcored main.cpp` (lines 306-311) and passed to QuestManager, but `CanComplete()`/`NewlyAvailable()` are not wired into detection yet.

**Why**: Duplicated logic violates DRY. QuestGraph already has correct DAG traversal. Using QuestGraph ensures consistent unlock behavior across server and client.

**Status**: Tasks 1.1 (LoadGraph) + 1.2 (prereq map) done; task 5.6 (swap inline → QuestGraph) + 5.10 (publish `QuestUnlocked`) **deferred** (functionally equivalent; no tests to guard the swap).

**Implementation**: 
1. ✔ `QuestData::LoadGraph()` parses quest_graph.json into `graph_` map + validates against CSV
2. ✔ prereq map via existing `GetPrerequisites()` + `Graph()`; `QuestGraph::Init()` wired in `simulation_core/main.cpp`
3. `BuildQuestEraMap()` — **deferred** (task 1.3, needed by `IsEraComplete()`)
4. ✔ Init QuestGraph with these maps in main.cpp (QuestManager ctor takes `QuestGraph*`)
5. Replace inline checks with `QuestGraph::CanComplete()` — **deferred**
6. After each completion, call `QuestGraph::NewlyAvailable()` for unlock events — **deferred**

### Decision 3: Topic split — quest.completed vs quest.completed.notification  [IMPLEMENTED]
**D3 (final)**: `quest.completed` carries only the FlatBuffer `QuestCompleted` from SimulationCore → MetaDB (SimCore also publishes `quest.progress.updated` with `QuestProgressUpdate`). MetaDB persists the reward and re-publishes a FlatBuffer `QuestCompletedNotification` on the separate `quest.completed.notification` topic → Gateway → Client. This keeps the raw event stream clean and lets Gateway map `quest.completed.notification` to `kQuestCompletedNotification` (type 21).

**Why**: The original spec had MetaDB publishing the notification back on `quest.completed`, creating a loop hazard (SimCore and MetaDB both publishing on the same topic). The split makes the direction explicit.

**Data flow**: Progress pull: SimCore → `meta_db.quest.get` (raw `[player_id:8 LE]`) → MetaDB → `meta_db.quest.get.response` (raw `[player_id:8 LE][n:2 LE][(quest_id,status,progress)*n]`).

**Manual completion (D1)**: routed to SimCore `QuestManager` for validation (`QuestGraph::CanComplete()`), not directly to MetaDB. MetaDB stays dumb persistence — it SHALL NOT fabricate `quest.completed` on `meta_db.quest.set`. **Deferred** — no client→server quest message type exists yet.

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
`QuestManager::distributeRewards()` logs the reward and publishes a `QuestCompleted` event (player_id, quest_id, timestamp) on `quest.completed`. The FlatBuffer `QuestCompleted` carries **no reward fields** — MetaDB resolves the reward from `quests.csv` via `GetQuestDefinition()` in `HandleQuestCompleted`, stores it in `player_quest_rewards`, and forwards a `QuestCompletedNotification` to the client.

**Why**: The reward data lives in the quest definition (single source of truth = `quests.csv`); shipping it redundantly in the event would risk divergence. Reward → inventory insertion is deferred until the inventory system is ready (separate change).

> ⚠️ **Schema note**: `QuestCompletedNotification` (quest.fbs) DOES carry `reward_item_id`/`reward_count`; `QuestCompleted` (event) does NOT.

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

1. ✔ Fix `LoadGraph()` stub — parses quest_graph.json, validates vs CSV
2. ✔ Prereq map + `QuestGraph::Init()` wired in simcored main (`BuildQuestEraMap()` deferred)
3. ✔ Wire QuestManager into CraftRequestHandler + SetBlockCASHandler + PlayerJoinedHandler
4. ⏳ Replace inline prereq checks with QuestGraph calls (deferred — task 5.6)
5. ⏳ Add TOOL_CHARGED + SIDE_CONFIGURED handlers (deferred — no quests use them)
6. ⏳ Wire era transition (IsEraComplete) — deferred (needs BuildQuestEraMap)
7. ✔ Fix GetQuestDefinition() in MetaDB — CSV-loaded map (`definitions.go`)
8. ⏳ Refactor QuestBookWindow: FlatBuffers parsing + manual complete + unlock notifications (client phase)
9. ⏳ Wire reward distribution to inventory system (deferred — separate change)

## Open Questions

- Should QuestManager be in a background thread or on the simulation tick? Currently uses mutex — thread-safe but may need integration with ECS tick.
- QuestGraph local computation on client: do we need server-authoritative unlock or is local computation sufficient? Trusted client model vs anti-cheat?
- Quest data editing: should quests.csv be editable at runtime or only at build time? Currently build-time only (loaded at startup).
- **MetaDB dual-handler inconsistency (D2, deferred)**: `quest_handlers.go` has FlatBuffers `HandleQuestGet`/`HandleQuestSet` (topics `quest.get`/`quest.set`) that are **dead code**; the registered raw-binary handlers live in `router_client.go` (`meta_db.quest.get`/`meta_db.quest.set`). Decide whether to delete the FlatBuffers handlers or migrate the raw path to FlatBuffers.
- **Manual completion has no wire path**: `gateway.fbs` has no client→gateway quest request type, and `on_client_ctrl_message` has no quest case. D1 (validate in SimCore) needs a new client→SimCore quest message type — undefined in schema.
- **detect_target id scheme**: now flat recipe output ids (verified: 33/35/14/36/37 match `crafting_table.yaml`). QuestManager compares `std::to_string(itemId)` where itemId is the recipe output id — the `uint16` item model must match recipe ids, not registry hierarchical ids.
