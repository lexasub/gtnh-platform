# Design: Init Game Flow (server-authoritative scenario execution)

## Context

A player joins with an empty inventory, `InventoryState::gameMode = CREATIVE` (flight, block
break/place allowed), and no guided start. We want `/startGameScenario 0` to clear the inventory,
grant a starter set, switch to SURVIVAL, and open the quest book on the Vagrant era.

Existing infrastructure that this change reuses:
- `ConsoleWindow` command registry (`RegisterCommand`) with a strict-validation precedent
  (`/gamemode`).
- Game-mode wire already exists end-to-end: `kGameModeChange`(30) → gateway
  `player.gamemode.change` → `PlayerInventoryStore::setGameMode` + echo back
  (`SimCoreMessageHandler.cpp:268-287`). Server already *stores and echoes* mode; it never
  *validates* it.
- Inventory pushes are already server-authoritative: `PlayerInventoryStore::setSlots` fires
  `onChange` (→ `meta_db.inventory.set`) and `postMutation` (→ full snapshot on
  `player.inventory.update` → gateway → `kInventoryUpdate`(6)).
- Quest book defaults `selectedEra_ = 0` (VAGRANT), so "show Vagrant" is a programmatic open, not
  a new tab.
- Items registry is packed-only (`ItemId::pack`); flat ids do not exist in `items.csv`.
  crafting_table = `0:10:11:1` → 22529; wooden_pickaxe = `0:11110:3` → 30723.

## Goals / Non-Goals

- Goals:
  - Scenario execution is server-authoritative: server owns the scenario contents and applies
    inventory + mode atomically; client is a thin requester.
  - Reuse existing wire and push paths wherever possible (no new inventory protocol).
  - Scenario is data, not code: a small table, extensible to future scenarios.
  - The client's inventory arrives from the authoritative `player.inventory.update` snapshot, so
    there is no client-side inventory mutation to race against server pushes.
- Non-Goals:
  - Permission/authorization on the command (any connected client may call it).
  - Sub-eras (data-driven era hierarchy).
  - Server-authoritative *validation* of arbitrary game-mode switches (that is the `add-game-modes`
    beta flip, unchanged).
  - Scenario persistence / cross-scenario inventory.

## Decisions

### D1: Server executes the scenario; client only requests
The original proposal had the client clear + grant locally and sync through existing channels.
That path cannot be reliable: there is no atomic "clear/replace" inventory operation in the
protocol (`InventoryAction` only has PICKUP/DROP/TAKE/PLACE/CRAFT/USE/BREAK/INSERT/REMOVE/
HOTBAR_CHANGE — see `simulation_core/InventoryActionHandler.h:45-60`), and the server re-pushes `InventoryUpdate`
snapshots that would overwrite local edits. A client-side clear means ~40 individual DROP actions,
non-atomic and racy with in-flight snapshots.

The server already has the atomic primitive: `PlayerInventoryStore::setSlots` (full replace) +
`setGameMode`. So the scenario handler in SimulationCore calls those, and the existing
`postMutation` push carries the authoritative snapshot to the client.

Alternatives considered:
- Client-side execution (original proposal) — rejected: no atomic clear, race with server pushes.
- Client sends the *full desired inventory* in the request and server applies it — rejected for
  now: it makes the server blindly trust client content, which buys nothing over the table-based
  approach and leaks the cheat window wider. If scenarios need runtime parameters later, the request
  can grow a payload; the execution path is the same.

### D2: Scenario = server-side data table; client sends only the index
`StartScenarioReq` carries `scenario_index`. The server owns the mapping index → contents in
`simulation_core/Scenario/GameScenario.h/.cpp`:

```cpp
struct GameScenario {
  uint8_t index;
  std::string name;
  Protocol::GameMode targetMode;
  std::vector<std::pair<uint16_t, uint8_t>> giveItems; // (packed_item_id, count)
  bool clearFirst;                                      // scenario 0: true
  uint8_t questBookEra;                                 // 0 = VAGRANT
};
inline const std::vector<GameScenario>& scenarios();     // static, lookup by index
```

The client keeps a display-only table (name, questBookEra, outputMessage) for `/help` and console
output. Authoritative contents live on the server. Scenario 0 = { clearFirst, giveItems =
[(22529,1),(30723,1)], targetMode = SURVIVAL, questBookEra = VAGRANT }.

### D3: Inventory sync reuses the existing push path; ordering is by construction
Execution order in the handler: `setSlots(empty)` → `giveItem` × N → `setGameMode` → publish
`player.scenario.start.response`. `setSlots`/`giveItem` fire `postMutation` synchronously
(header `PlayerInventoryStore.h:43-49`), so `player.inventory.update` is enqueued **before** the
response is published. The client therefore receives the authoritative snapshot (via
`kInventoryUpdate`=6) ahead of the scenario ack. Two snapshots may arrive (empty, then
empty+grant) because `setSlots` and `giveItem` each publish once — both are authoritative and the
last one wins; harmless.

### D4: Game mode travels in the response, not a second echo
The handler calls `setGameMode` (server state) but does **not** re-publish the existing
`player.gamemode.changed` echo. `StartScenarioResp` carries `game_mode`; the client applies it to
`InventoryState::gameMode` on success. This keeps the ack self-contained (mode + quest era in one
message) and avoids double-apply of the same mode via two paths. When `add-game-modes` later makes
the mode server-authoritative, this handler already owns the server-side store write.

### D5: Wire indices 31/32, fbs union is known-stale
The next free wire constants are `kStartScenarioReq = 31`, `kStartScenarioResp = 32`
(after `kGameModeChange = 30`, `gateway.h:62`). The `gateway.fbs` union indices are stale vs the C++
constants (documented at `gateway.fbs:22-27`); the C++ `GatewayMsg` constants govern the wire.
Both the fbs union entries and the C++ constants change together in this change.

### D6: No new inventory protocol
The scenario produces no new message types for inventory. Granting/clearing uses existing
`setSlots`/`giveItem` semantics; the client never mutates slots locally for a scenario. This is
what makes the P0 inventory race disappear — there is only one writer (the server).

### D7: Quest book open is client-side, driven by the response
On a successful `StartScenarioResp`, the client opens `QuestBookWindow` programmatically
(`uiMgr_->Find<QuestBookWindow>()` → `SetOpen(true)`) and selects the era from the response via a
new `SetEra(int)`. `selectedEra_` already defaults to VAGRANT (0), so the select is a no-op for
scenario 0 but future scenarios can target other eras.

## Risks / Trade-offs

- **Partial execution on crash**: `setSlots`, `giveItem`, `setGameMode` are sequential store calls,
  not one transaction. A crash between them leaves partially-applied state in MetaDB (per-slot
  `onChange` writes). → Accepted for dev: scenario 0 is idempotent to re-run (clear-first), and
  the authoritative snapshot heals the client. A single-lock `applyScenario()` on
  `PlayerInventoryStore` is the noted future hardening.
- **Double inventory snapshot** (empty, then granted): benign, both authoritative.
- **Any client can trigger a scenario**: accepted, matches the dev-phase trust model of
  `add-game-modes`. Authorized later by a permission gate on the handler.
- **Tool prefix inconsistency**: `items.csv` defines manual tools under `0:11110` (packed 0x78xx),
  but `ItemId.h::toolTier`/`toolType` expect the `1111:00` prefix (0xF0xx–0xF3xx). Pre-existing
  registry inconsistency, not touched here; the scenario grants the packed id as-is.

## Migration Plan

- Additive protocol change (two new union members + C++ constants). No data migration.
- FlatBuffers regenerated from fbs; both fbs and C++ constants in the same change.
- No changes to `quests.csv` / `quest_graph.json` / `items.csv`.

## Open Questions

- Should the grant include a stone pickaxe instead of wooden (progression intent)? Default: wooden,
  matching the "first steps" quest arc; trivially changed in the data table.
- Should the response also carry the granted items so the client can print "you received X" without
  a client-side table? Default: no — console output is display-only and may diverge.
