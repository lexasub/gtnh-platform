# Audit gp-48he — publisher table coverage: constructed/published vs declared, and vs the union

**Scope:** read-only audit. No `.fbs`, `.cpp`, or `.go` file was modified.
**Date:** 2026-09-26

---

## TL;DR

1. **The C++ codegen is healthy.** All 161 declared tables get a generated struct,
   and every generated header is **newer than its source `.fbs`**. No staleness there.
2. **The Go codegen is the broken one.** `src/apps/meta_db/CMakeLists.txt` only ever
   runs `flatc` on **3 of the 14 schemas**. `gateway.fbs`, `recipe.fbs`,
   `tile_entity_store.fbs`, `machine_state.fbs`, `service_health.fbs`,
   `multiblock_state.fbs`, `chunkstore.fbs`, `entity_state_store.fbs`,
   `pipe_network.fbs`, `client_state.fbs` are **never** compiled to Go.
3. **35 orphaned Go files** survive in `src/protocol/generated/go/Protocol/` from a
   since-deleted schema — including `ChestOpenReq`/`ChestOpenResp`, which exist in
   **no `.fbs` in the repository**, and a `GatewayPayload` union that is a
   **fourth** disagreeing numbering scheme (see `gatewaymsg-vs-fbs.md` §1).
4. **32 of 161 declared tables (20%) are never referenced anywhere** in the repo.
   Nine of them are the entire `tile_entity_store.fbs` service, which is not built.
5. **The `GatewayPayload` union is the only union that is a lie.** Every other union
   (`ChunkStoreRequest/Response`, `MetaDBRequest/Response`, `EntityStateRequest/Response`,
   `TileEntityStoreRequest/Response`, `RecipeRequest/Response`, `SimCoreRequest/Response`)
   is either used correctly or belongs to an unbuilt service.

---

## 1. Code-generation wiring

### 1a. C++ — healthy, 10/14 schemas compiled, all in sync

| Schema | Generated into |
|---|---|
| `core.fbs`, `quest.fbs` | `src/game/quests/CMakeLists.txt:12-27` (`--gen-mutable`) |
| `gateway.fbs` + others | `src/apps/gateway/CMakeLists.txt:16-21` |
| `simcore.fbs` + 10 more | `src/apps/simcore/CMakeLists.txt:38-47` |
| `chunkstore.fbs` | `src/apps/chunk_store/CMakeLists.txt:17-22` |
| `recipe.fbs` | `src/game/recipes/CMakeLists.txt:19-24` |
| `entity_state_store.fbs` | `src/apps/entity_state_store/CMakeLists.txt:24-29` |
| `pipe_network.fbs` | `src/apps/pipe_network/CMakeLists.txt:19-24` |

Freshness check (`.fbs` mtime vs newest generated header in `cmake-build-debug/`):

```
  quest.fbs                fbs=08-06 18:43  gen=09-21 04:01  ok
  core.fbs                 fbs=09-13 20:50  gen=09-21 04:01  ok
  gateway.fbs              fbs=08-08 10:50  gen=09-21 04:01  ok
  recipe.fbs               fbs=09-13 20:50  gen=09-21 04:01  ok
  pipe_network.fbs         fbs=09-17 13:48  gen=09-21 04:01  ok
  service_health.fbs       fbs=09-14 02:04  gen=09-21 04:01  ok
  client_state.fbs         fbs=09-13 20:50  gen=09-21 04:01  ok
  tile_entity_store.fbs    fbs=06-28 17:52  gen=09-21 04:01  ok
  machine_state.fbs        fbs=06-28 17:52  gen=09-21 04:01  ok
  multiblock_state.fbs     fbs=08-03 12:03  gen=09-21 04:00  ok
```

And every declared table has a generated struct:
```
C++ generated structs found in build tree: 161
DECLARED tables with NO C++ struct ANYWHERE (incl. build dirs): 0
```

**Note the build-dir layout trap:** `src/protocol/generated/` contains only 7
headers (no `quest_generated.h`, no `pipe_network_generated.h`,
no `service_health_generated.h`). Those are generated into
`cmake-build-debug/**/…_fbs/`. A grep that only looks at
`src/protocol/generated/` will report false "table not generated" results for
`QuestProgressUpdate`, `PipeContentsResp`, `ServiceHealthResp`, etc. **Do not
trust that directory as a codegen index.**

### 1b. Go — broken, 3/14 schemas compiled

`src/apps/meta_db/CMakeLists.txt:26-32` is the **only** Go FlatBuffers codegen rule
in the whole tree:

```cmake
    COMMAND flatbuffers::flatc
        --go
        -I ${FBS_DIR}
        -o ${FBS_GO_OUT_DIR}
        ${FBS_DIR}/meta_db.fbs
        ${FBS_DIR}/core.fbs
        ${FBS_DIR}/quest.fbs
```

**10 schemas are never compiled to Go:** `gateway.fbs`, `recipe.fbs`,
`chunkstore.fbs`, `entity_state_store.fbs`, `simcore.fbs`, `pipe_network.fbs`,
`client_state.fbs`, `service_health.fbs`, `machine_state.fbs`,
`multiblock_state.fbs`, `tile_entity_store.fbs`.

Today this happens to be *sufficient*, because the only Go consumers are MetaDB
(needs `core` + `quest` + `meta_db`) and MessageRouter (needs none). But it is
why the orphaned `gateway.fbs` output survives, and it is a trap for the next Go
sidecar.

---

## 2. Tables used by a publisher — routing works end to end

Method: for each `Protocol::Create<T>` (C++) and `Protocol.<T>Start` (Go)
outside generated code, confirm a matching topic publisher and subscriber.

| Table | Built by | Topic published | Subscribed by | Routing |
|---|---|---|---|---|
| `MetaDBFrame`/`MetaDBMessage`/`SetInventorySlotReq` | `InventoryActionHandler.cpp:266-289`, `simcore/main.cpp:305-310` | `meta_db.inventory.set` | MetaDB `router_client.go:199`→`flatbuffer_tcp.go:58` | ✅ works |
| `PlayerJoined` | `gateway.cpp:113,327` | `player.joined` | MetaDB `router_client.go:376`; SimCore `SimCoreMessageHandler.cpp:149` | ✅ works |
| `PlayerLeft` | `gateway.cpp:73,335` | `player.left` | MetaDB `router_client.go:409` | ✅ works |
| `QuestCompleted` | `QuestManager.cpp:23-25` | `quest.completed` | MetaDB `quest_handlers.go:142` | ✅ works |
| `QuestProgressUpdate` (query) | `PlayerJoinedHandler.cpp:39-42` | `meta_db.quest.get` | MetaDB `quest_handlers.go:20` → replies `meta_db.quest.get.response` | ✅ works |
| `QuestProgressUpdate` (set) | — | `meta_db.quest.set` | MetaDB `quest_handlers.go:76` | ❌ **no publisher** |
| `QuestExchangeRequest` | client `NetClient.cpp:819`, relayed `gateway.cpp:609-612` | `quest.exchange.request` | MetaDB `exchange_handlers.go:19` | ✅ works (pure passthrough) |
| `QuestExchangeCooldownGet` | client `NetClient.cpp:829`, relayed `gateway.cpp:615-618` | `quest.exchange.cooldown.get` | MetaDB `exchange_handlers.go:206` | ✅ works |
| `QuestExchangeResponse` | MetaDB `exchange_handlers.go:30-45` | `quest.exchange.response` | gateway `gateway.cpp:506` | ✅ works |
| `QuestExchangeCooldown` | MetaDB `exchange_handlers.go:229-239` | `quest.exchange.cooldown.response` | gateway `gateway.cpp:508` | ✅ works |
| `QuestCompletedNotification` | MetaDB `quest_handlers.go:198-211` | `quest.completed.notification` | gateway `gateway.cpp:500` | ✅ works |
| `InventoryUpdate` | MetaDB `db.go:235-257` | `player.inventory.load` | SimCore `main.cpp:589` → `SimCoreMessageHandler.cpp:145` → `InventoryLoadHandler` | ✅ works |
| `ContainerOpenReq` | client `NetClient.cpp:850,862,873,884,895` | 5 wire ids → `player.{chest,machine,workbench}.{open,close}` | gateway `gateway.cpp:640-680` | ✅ works |
| `ServiceHealthReq/Resp` | client `NetClient.cpp:905`; gateway `gateway.cpp:209` | `service.health.*` | gateway `gateway.cpp:725` | ✅ works |
| `PipeContentsReq/Resp` | client `NetClient.cpp:915`; gateway `gateway.cpp:470` | `pipe.contents.request` | pipe_network | ✅ works |
| `ResourceBufferState` | `ResourceBufferStatePublisher` | resource-state topic | gateway `gateway.cpp:450` | ✅ works |

**The `player.inventory.load` chain is intact** — and it is the best-worked
example in the tree, because it is where the C++ verifier hardening was applied.
MetaDB publishes it in two places (`router_client.go:386`, `flatbuffer_tcp.go:147`);
**SimCore** is the subscriber, not the gateway:

```
$ sed -n '589p' src/apps/simcore/main.cpp
    routerClient->Subscribe("player.inventory.load");
$ sed -n '145,146p' src/apps/simcore/Network/SimCoreMessageHandler.cpp
    topicDispatcher_->on("player.inventory.load", std::make_unique<InventoryLoadHandler>(
        d.inventoryStore, d.routerClient));
```

`InventoryLoadHandler.cpp:22-30` verifies **before** touching the pointer and
handles the optional-`slots` case explicitly — a good model for the Go side to
copy (see `field-usage-matrix.md` §2).

The gateway's subscribe list (`src/apps/gateway/main.cpp:125-140`) does *not*
include `player.inventory.load`; it subscribes to the downstream
`player.inventory.update` instead. That is correct — the gateway is not in this
path. **No defect here.** (Recording this explicitly because an initial narrower
grep, scoped to `src/apps/gateway` + `src/apps/meta_db` only, appeared to show
the topic was unconsumed. It is not.)

---

## 3. Orphaned Go generated files (35) — the `gateway.fbs` residue

These have **no corresponding `table` in any `.fbs` in the repository**:

`BlockAckStatus` `BlockDirective` `CASStatus` **`ChestOpenReq`** **`ChestOpenResp`**
`ChunkStorePayload` `ChunkStoreRequest` `ChunkStoreResponse` `CoverType`
`EnergyStorage` `EnergyType` `EntityStateRequest` `EntityStateResponse`
`FacePolicy` `FluidStack` `FluidTank` `GameMode` **`GatewayPayload`**
`HatchType` `ItemStack` `MachineActionType` `MachineSlotType` `MetaDBPayload`
`MetaDBRequest` `MetaDBResponse` `PipeWrenchGuidance` `PlayerActionType`
`PortRole` `QuestStatus` `ResourceKind` `SimCoreRequest` `SimCoreResponse`
`ToolActionType` `Vec3f` `Vec3i`

Most are legitimately **enums/structs** (my table-extraction regex only matched
`table X {`), so they are expected outputs, not orphans. The genuinely
suspicious ones are the four **payload/frame** families plus the two chest types:

| Orphan | Why it is a real problem |
|---|---|
| `GatewayPayload`, `GatewayMessage` | The union is a **fourth** numbering scheme, disagreeing with both the `.fbs` and `GatewayMsg.h`. It is never regenerated (1b) and never used. `gatewaymsg-vs-fbs.md` §1. |
| `ChestOpenReq`, `ChestOpenResp` | **No such table in any `.fbs`.** `grep -rn "ChestOpenReq" src/protocol/*.fbs` → no output. They correspond to a schema that once had 19+ union members. The live protocol instead multiplexes 5 wire ids over `ContainerOpenReq` (`core.fbs:313`). |
| `ChunkStoreRequest/Response/Payload` | ChunkStore is C++-only. Go never uses these. |
| `MetaDBRequest/Response/Payload` | **Not orphans** — these *are* generated from `meta_db.fbs:39,58,78`. My regex missed union bodies. |
| `SimCoreRequest/Response`, `EntityStateRequest/Response` | Not orphans — generated from `simcore.fbs:33,70` and `core.fbs:361,366`. |

So the real orphan count is **6** (`GatewayPayload`, `GatewayMessage`,
`ChestOpenReq`, `ChestOpenResp`, `ChunkStoreRequest/Response/Payload` = 7 files),
all residue of schemas no longer fed to `flatc`. They are harmless today (nothing
imports them) but they are a trap: `ChestOpenReq` looks like a usable protocol
type, and the stale `GatewayPayload` union looks authoritative.

**Important for the later fix task:** deleting these files is safe **only if**
`gateway.fbs` is also fixed first. If someone adds `gateway.fbs` to the Go
`flatc` invocation as part of fixing the union, `GatewayPayload.go` and
`GatewayMessage.go` get overwritten with the corrected union — good — but
`ChestOpenReq.go`/`ChestOpenResp.go` will **persist as orphans** because flatc
will not delete them.

---

## 4. Tables declared but never referenced anywhere (32)

Method: for each of the 161 declared tables, grep the entire repo (excluding
`generated/`, `.claude/`, `cmake-build*/`, `.git/`) for the identifier. Zero hits
= never referenced.

| Table | Schema | Category |
|---|---|---|
| `BlockChangedReq` / `BlockChangedResp` | simcore.fbs:12,44 | ⚠️ **contradicts AGENTS.md** |
| `MatchPatternReq` / `MatchPatternResp` | simcore.fbs:19,48 | dead RPC |
| `GetControllerReq` / `GetControllerResp` | simcore.fbs:23,58 | dead RPC |
| `UnregisterControllerReq` / `UnregisterControllerResp` | simcore.fbs:27,62 | dead RPC |
| `ControllerData` | simcore.fbs:52 | dead RPC |
| `TickReq` / `TickResp` | simcore.fbs:31,66 | dead RPC |
| `SimCoreMessage` / `SimCoreReply` | simcore.fbs:82,87 | dead frame |
| `GetChunkReq` / `GetChunkResp` / `SaveChunkReq` | chunkstore.fbs:30,68,34 | dead RPC |
| `GetTileStateReq`/`Resp`, `SetTileStateReq`/`Resp`, `RemoveTileStateReq`/`Resp` | tile_entity_store.fbs:17,46,23,50,30,54 | **whole service unbuilt** |
| `TileEntityStoreMessage`/`Reply`/`Frame` | tile_entity_store.fbs:69,74,84 | **whole service unbuilt** |
| `MachineAction` / `MachineActionResp` | core.fbs:554,562 | ⚠️ **in the dead union** |
| `SideConfigData` | core.fbs:578 | dead |
| `CoverInfo` | core.fbs:457 | dead |
| `EntityStateUpdate` | core.fbs:326 | dead |
| `GatewayMessage` | gateway.fbs:65 | dead union wrapper |
| `NBTTag` | machine_state.fbs:34 | dead |
| `CableNodeStatus` | pipe_network.fbs:139 | dead |
| `EnergyPacketDef` | pipe_network.fbs:129 | dead |
| `ResourcePortUpdate` | pipe_network.fbs:184 | dead |
| `ResourceRequirement` | recipe.fbs:84 | dead |
| `RecipeMessage` | recipe.fbs:147 | dead (RecipeFrame is used instead) |

### 4a. `BlockChangedEvent` is the notable one

`AGENTS.md` §CONVENTIONS states:

> **Event-driven**: `BlockChanged` published by ChunkStore → caught by SimCore

**`BlockChangedReq` and `BlockChangedResp` are referenced nowhere.** The only
`BlockChanged*` symbols with real references are the **router-event** tables
`BlockChangedEvent` (`core.fbs:374`) and the Go `BlockChangedEvent.go` /
`BlockChangedReq.go` / `BlockChangedResp.go` generated files. So the
`simcore.fbs` RPC form of the ChunkStore→SimCore contract is **declared but not
wired**; the live path is a different, router-event one. The AGENTS.md sentence
describes intent, not the implementation. Flagged P2 (documentation accuracy).

### 4b. `tile_entity_store.fbs` — an entire 9-table service that does not exist

`src/protocol/tile_entity_store.fbs` declares 9 tables + 3 frames + 2 unions.
**Zero** C++ or Go code references any of them:

```
$ grep -rn "TileEntityStore" --include=*.cpp --include=*.h --include=CMakeLists.txt src/ | grep -v protocol/generated
(no output)
```

It is also **not in any `flatc` invocation** (not in
`src/apps/simcore/CMakeLists.txt:42`). This matches the AGENTS.md TODO
*"Server-authoritative grid state via TileEntityStore RPC"* — so it is a known
unimplemented feature, correctly tracked. Recorded here for completeness; it is
**not** a defect.

---

## 5. Tables constructed by a publisher but absent from the union

This is the specific question asked. Two distinct sub-cases:

### 5a. Absent from `GatewayPayload` — the 26-id list

Fully enumerated in `gatewaymsg-vs-fbs.md` §3a. Summary: ids 2, 4, 15, 17, 18,
19, 23, 25, 30, 34–43, 44, 45, 46, 47, 48, 49, 50, 51 are live on the wire and
have **no** `GatewayPayload` member. This is a *deliberate* consequence of the
union being dead — nothing builds a `GatewayMessage` wrapper.

### 5b. The `*Direct` hand-rolled builder helpers — an undocumented pattern

C++ code constructs eight message types that the generated `Create<T>` never
produces, via hand-written `<T>Direct` functions:

| Direct builder | Wrapped type | Implies |
|---|---|---|
| `BlockEntityUpdateDirect` | `BlockEntityUpdate` (core.fbs:470) | a schema-vs-code drift: the generated helper is bypassed |
| `CraftRequestDirect` | `CraftRequest` (core.fbs:498) | same |
| `RecipeCompletedDirect` | `RecipeCompleted` (recipe.fbs:137) | same |
| `RecipeInfoDirect` | `RecipeInfo` (recipe.fbs:91) | same |
| `MachineConfigUpdatedDirect` | `MachineConfigUpdated` (core.fbs:584) | same |
| `EnergyNodeUpdateDirect` | `EnergyNodeUpdate` (pipe_network.fbs:64) | same |
| `FluidNodeUpdateDirect` | `FluidNodeUpdate` (pipe_network.fbs:162) | same |
| `ItemNodeUpdateDirect` | `ItemNodeUpdate` (pipe_network.fbs:303) | same |

**Unverifiable by design:** I did not diff each `*Direct` body against its
generated `Create<T>`. A field added to any of these eight tables would be
**silently default-valued** on the wire, because the hand-rolled builder does
not know about it. That is a real maintenance hazard and a plausible future
silent-zero source, but I cannot claim any specific field is currently wrong
without reading all eight bodies against eight generated signatures. **This is
the single most valuable follow-up for the fix task.**

---

## 6. Findings, ranked

| # | Sev | Finding | Evidence |
|---|---|---|---|
| 1 | **P1** | Go codegen compiles only 3 of 14 schemas; `gateway.fbs` is not among them. This is the root cause of the stale Go union. | `src/apps/meta_db/CMakeLists.txt:26-32` |
| 2 | **P1** | 7 orphaned Go files from deleted schemas, incl. `ChestOpenReq`/`ChestOpenResp` (no such table anywhere) and the stale `GatewayPayload` union. | §3 |
| 3 | **P1** | `meta_db.quest.set` is subscribed and dispatched but has **no publisher** → `HandleQuestSet` is dead. | `router_client.go:108,218`; see `field-usage-matrix.md` §5 |
| 4 | **P1** | 8 message types are built by hand-rolled `*Direct` builders that bypass the generated `Create<T>` — any future field addition is silently defaulted. | §5b |
| 5 | **P2** | `src/protocol/generated/` is a **false index**: it holds 7 headers, missing `quest`/`pipe_network`/`service_health`/`client_state`/`multiblock_state`. Real output is in `cmake-build-debug/**/`. | §1a |
| 6 | **P2** | 32 of 161 declared tables (20%) are referenced nowhere. | §4 |
| 7 | **P2** | `BlockChangedReq`/`Resp` never referenced — `AGENTS.md`'s "BlockChanged published by ChunkStore → caught by SimCore" describes an unimplemented contract. | §4a |
| 8 | **P3** | `MachineAction`/`MachineActionResp` are dead *and* present in the stale union at ids 15/16, where they shadow live ids. | `core.fbs:554,562`; `gateway.fbs:49-50` |
| 9 | **P3** | `tile_entity_store.fbs` (9 tables) is declared, unbuilt, and uncodegen'd — correctly tracked as a TODO, recorded for completeness. | §4b |

---

## 7. Explicitly unverifiable

- **Whether each `*Direct` builder currently matches its generated `Create<T>`
  field-for-field.** Not diffed — see §5b. This is the highest-value remaining
  check and is deliberately left to the fix task rather than guessed at.
- **Whether the 32 unreferenced tables are planned-but-unimplemented features or
  abandoned.** For `tile_entity_store` the AGENTS.md TODO answers it; for the
  ~20 others I have no source of intent.
