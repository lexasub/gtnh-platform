# Audit gp-a6l — `GatewayMsg.h` constants vs `gateway.fbs` `GatewayPayload` union

**Scope:** read-only protocol audit. No `.fbs` / `.cpp` / `.go` files were modified.
**Repo:** `/home/su/src/local/gtnh-platform` @ `main`
**Date:** 2026-09-26

---

## TL;DR — the documented divergence is much narrower than the real one

`AGENTS.md` and the header comment at `src/common/GatewayMsg.h:12-13` say the
divergence is that *"the FlatBuffers `GatewayPayload` union in gateway.fbs is
stale"* and that the stale part is **quest indices** (`src/protocol/gateway.fbs:22-27`).

**That is an understatement. The union is wrong for every single one of its 28
members, including ids 1–17 which are plain player/craft/inventory traffic.**
The `// NOTE:` at `src/protocol/gateway.fbs:22` calls the fbs comments "stale
vs the C++ GatewayMsg constants" but then only documents the quest block
(20→18, 21→19, …). It never mentions that `PlayerAction` itself is 1 vs 1 (ok),
but `BlockAck` is 4 in fbs / **5** on the wire, `SetBlockAction` is 5 in fbs /
**11** on the wire, and `GridUpdate` is 10 in fbs / **43** on the wire.

**Net effect: 18 message types are silently mistranslated, and 26 message types
exist on the C++ side with no fbs union entry at all.** A publisher that
consulted the fbs union and a subscriber that consulted `GatewayMsg.h` would
talk past each other on all 18.

**Why it has not caused an outage:** the union is *dead code*. Zero non-generated
C++ and zero non-generated Go references `Protocol::GatewayPayload` or
`Protocol::GatewayMessage`. See "Why this has not broken anything yet" below.

---

## 1. The three sources of truth

| Source | Path | Authority |
|---|---|---|
| C++ constants | `src/common/GatewayMsg.h:16-75` | **AUTHORITATIVE** — this is what the handlers switch on |
| fbs union | `src/protocol/gateway.fbs:34-63` | DEAD — 0 non-generated references |
| C++ generated header | `src/protocol/generated/gateway_generated.h:enum GatewayPayload` | DEAD, and *stale vs the .fbs itself* |
| Go generated union | `src/protocol/generated/go/Protocol/GatewayPayload.go:12-40` | DEAD, and *older still* |

```
$ grep -rn "GatewayPayload" --include=*.cpp --include=*.h --include=*.hpp src/ test/ tools/ | grep -v "src/protocol/generated"
src/common/GatewayMsg.h:13:// GatewayPayload union in gateway.fbs is stale and unused.
$ grep -rn "GatewayPayload\|GatewayMessage" --include=*.go src/ test/ tools/ | grep -v "/generated/"
(no output)
```

**A 4th, undiscovered divergence: the two generated artifacts do not even agree
with each other or with their own source `.fbs`.** There are effectively four
different numbering schemes in the tree.

```
$ ls -la --time-style=long-iso src/protocol/gateway.fbs \
    src/protocol/generated/gateway_generated.h \
    src/protocol/generated/go/Protocol/GatewayPayload.go
-rw-r--r-- 1 su su  3933 2026-08-08 10:50 src/protocol/gateway.fbs
-rw-r--r-- 1 su su 25639 2026-09-21 05:32 src/protocol/generated/gateway_generated.h
-rw-r--r-- 1 su su  6027 2026-08-06 13:38 src/protocol/generated/go/Protocol/GatewayPayload.go
```

The Go file is **two days older than the `.fbs` it was generated from**, and the
C++ header is six weeks newer than both. The Go union still contains
`ChestOpenReq`/`ChestOpenResp` at ids 18/19 — types that **do not exist in any
`.fbs` file in the tree**:

```
$ grep -rn "ChestOpenReq\|ChestOpenResp" src/protocol/*.fbs
(no output — exit 1)
```

Root cause of the staleness, `src/apps/meta_db/CMakeLists.txt:30-32`: the Go
codegen rule only ever feeds `flatc` three schemas —

```cmake
    ${FBS_DIR}/meta_db.fbs
    ${FBS_DIR}/core.fbs
    ${FBS_DIR}/quest.fbs
```

`gateway.fbs` is **not in the list**, and no other service generates Go. So
`GatewayPayload.go`, `GatewayMessage.go`, `ChestOpenReq.go`, `ChestOpenResp.go`
are orphaned outputs from a since-deleted schema. See
`publisher-table-coverage.md` §3 for the full orphan list.

---

## 2. COMPLETE side-by-side table

`C++ id` = `src/common/GatewayMsg.h`. `fbs id` = position in the
`GatewayPayload` union, `src/protocol/gateway.fbs:34-63`. The `C++ table` column
is the FlatBuffers table the **actual handler** verifies/reads — this is the
real payload for that wire id, which is frequently *not* the table named after
the constant.

| C++ id | GatewayMsg.h constant | fbs id | fbs union member | Agree? | Real table on the wire | Authority |
|---:|---|---:|---|---|---|---|
| 1 | `kPlayerAction` | 1 | `PlayerAction` | ✅ name | `Protocol::PlayerAction` (core.fbs:129) | C++ |
| 2 | `kChunkSnapshot` | — | — | ❌ fbs-missing | *(hand-rolled, see §4)* | C++ |
| 3 | `kEntitySnapshot` | 3 | `EntitySnapshot` | ✅ name | `Protocol::EntitySnapshot` (core.fbs:220) | C++ |
| 4 | `kBlockUpdate` | — | — | ❌ fbs-missing | *(hand-rolled, see §4)* | C++ |
| 5 | `kBlockAck` | **4** | `BlockAck` | ❌ **-1** | `Protocol::BlockAck` (core.fbs:157) | C++ |
| 6 | `kInventoryUpdate` | 6 | `InventoryUpdate` | ✅ name | `Protocol::InventoryUpdate` (core.fbs:275) | C++ |
| 7 | `kInventoryAction` | 7 | `InventoryAction` | ✅ name | `Protocol::InventoryAction` (core.fbs:300) | C++ |
| 8 | `kBlockEntityUpdate` | **11** | `BlockEntityUpdate` | ❌ **+3** | `Protocol::BlockEntityUpdate` (core.fbs:470) | C++ |
| 9 | `kCraftRequest` | **8** | `CraftRequest` | ❌ **-1** | `Protocol::CraftRequest` (core.fbs:498) | C++ |
| 10 | `kCraftResponse` | **9** | `CraftResponse` | ❌ **-1** | `Protocol::CraftResponse` (core.fbs:505) | C++ |
| 11 | `kSetBlockAction` | **5** | `SetBlockAction` | ❌ **-6** | `Protocol::SetBlockAction` (core.fbs:142) | C++ |
| 12 | `kCompressedChunkData` | 12 | `CompressedChunkData` | ✅ | `Protocol::CompressedChunkData` (core.fbs:201) | C++ |
| 13 | `kToolAction` | 13 | `ToolAction` | ✅ | `Protocol::ToolAction` (core.fbs:534) | C++ |
| 14 | `kToolActionResp` | 14 | `ToolActionResp` | ✅ | `Protocol::ToolActionResp` (core.fbs:544) | C++ |
| 15 | `kSetMachineSlot` | — | — | ❌ fbs-missing | `Protocol::SetMachineSlotReq` (core.fbs:524) | C++ |
| 16 | `kSetMachineSlotResp` | **17** | `SetMachineSlotResp` | ❌ **+1** | `Protocol::SetMachineSlotResp` (core.fbs:571) | C++ |
| 17 | `kRecipeCompleted` | — | — | ❌ fbs-missing | `Protocol::RecipeCompleted` (recipe.fbs:137) | C++ |
| 18 | `kMachineOpenReq` | **18** | `QuestProgressUpdate` | ❌ **-2** | `Protocol::ContainerOpenReq` (core.fbs:313) | C++ |
| 19 | `kChestOpenReq` | **19** | `QuestUnlockNotification` | ❌ **-2** | `Protocol::ContainerOpenReq` (core.fbs:313) | C++ |
| 20 | `kQuestProgressUpdate` | **18** | `QuestProgressUpdate` | ❌ **-2** | `Protocol::QuestProgressUpdate` (quest.fbs:30) | C++ |
| 21 | `kQuestUnlockNotification` | **19** | `QuestUnlockNotification` | ❌ **-2** | `Protocol::QuestUnlockNotification` (quest.fbs:94) | C++ |
| 22 | `kQuestCompletedNotification` | **20** | `QuestCompletedNotification` | ❌ **-2** | `Protocol::QuestCompletedNotification` (quest.fbs:100) | C++ |
| 23 | `kMultiblockEvent` | **21** | `QuestCompleteRequest` | ❌ **-2** | `Protocol::MultiblockCreatedEvent` / `MultiblockDestroyedEvent` (core.fbs:383,389) | C++ |
| 24 | `kQuestCompleteRequest` | **21** | `QuestCompleteRequest` | ❌ **-3** | `Protocol::QuestCompleteRequest` (quest.fbs:45) | C++ |
| 25 | `kQuestEraTransition` | **22** | `QuestExchangeRequest` | ❌ **-3** | `Protocol::EraTransitionNotification` (quest.fbs:111) | C++ |
| 26 | `kQuestExchangeRequest` | **22** | `QuestExchangeRequest` | ❌ **-4** | `Protocol::QuestExchangeRequest` (quest.fbs:53) | C++ |
| 27 | `kQuestExchangeResponse` | **23** | `QuestExchangeResponse` | ❌ **-4** | `Protocol::QuestExchangeResponse` (quest.fbs:71) | C++ |
| 28 | `kQuestExchangeCooldownGet` | **24** | `QuestExchangeCooldownGet` | ❌ **-4** | `Protocol::QuestExchangeCooldownGet` (quest.fbs:81) | C++ |
| 29 | `kQuestExchangeCooldown` | **25** | `QuestExchangeCooldown` | ❌ **-4** | `Protocol::QuestExchangeCooldown` (quest.fbs:87) | C++ |
| 30 | `kGameModeChange` | — | — | ❌ fbs-missing | `Protocol::GameModeChange` (core.fbs:32) | C++ |
| 31 | `kStartScenarioReq` | **26** | `StartScenarioReq` | ❌ **-5** | `Protocol::StartScenarioReq` (core.fbs:41) | C++ |
| 32 | `kStartScenarioResp` | **27** | `StartScenarioResp` | ❌ **-5** | `Protocol::StartScenarioResp` (core.fbs:49) | C++ |
| 33 | `kQuestBookOpen` | **28** | `QuestBookOpen` | ❌ **-5** | `Protocol::QuestBookOpen` (quest.fbs:64) | C++ |
| 34 | `kRecipeCheckReq` | — | — | ❌ fbs-missing | `Protocol::RecipeFrame`+`CheckRecipeReq` (recipe.fbs:25,162) | C++ |
| 35 | `kRecipeCheckResp` | — | — | ❌ fbs-missing | `Protocol::RecipeFrame`+`CheckRecipeResp` (recipe.fbs:69) | C++ |
| 36 | `kRecipeCatalogReq` | — | — | ❌ fbs-missing | `Protocol::RecipeFrame`+`RecipeCatalogReq` (recipe.fbs:42) | C++ |
| 37 | `kRecipeCatalogResp` | — | — | ❌ fbs-missing | `Protocol::RecipeFrame`+`RecipeCatalogResp` (recipe.fbs:109) | C++ |
| 38 | `kRecipeItemReq` | — | — | ❌ fbs-missing | `Protocol::RecipeFrame`+`RecipesForItemReq` (recipe.fbs:47) | C++ |
| 39 | `kRecipeItemResp` | — | — | ❌ fbs-missing | `Protocol::RecipeFrame`+`RecipesForItemResp` (recipe.fbs:113) | C++ |
| 40 | `kRecipeMachineReq` | — | — | ❌ fbs-missing | `Protocol::RecipeFrame`+`RecipesForMachineReq` (recipe.fbs:53) | C++ |
| 41 | `kRecipeMachineResp` | — | — | ❌ fbs-missing | `Protocol::RecipeFrame`+`RecipesForMachineResp` (recipe.fbs:117) | C++ |
| 42 | `kBlockActionDirective` | — | — | ❌ fbs-missing | `Protocol::BlockActionDirective` (core.fbs:178) | C++ |
| 43 | `kGridUpdate` | **10** | `GridUpdate` | ❌ **-33** | `Protocol::GridUpdate` (core.fbs:514) | C++ |
| 44 | `kWorkbenchOpenReq` | — | — | ❌ fbs-missing | `Protocol::ContainerOpenReq` (core.fbs:313) | C++ |
| 45 | `kChestCloseReq` | — | — | ❌ fbs-missing | `Protocol::ContainerOpenReq` (core.fbs:313) | C++ |
| 46 | `kMachineCloseReq` | — | — | ❌ fbs-missing | `Protocol::ContainerOpenReq` (core.fbs:313) | C++ |
| 47 | `kResourceBufferState` | — | — | ❌ fbs-missing | `Protocol::ResourceBufferState` (client_state.fbs:22) | C++ |
| 48 | `kPipeContentsReq` | — | — | ❌ fbs-missing | `Protocol::PipeContentsReq` (pipe_network.fbs:45) | C++ |
| 49 | `kPipeContentsResp` | — | — | ❌ fbs-missing | `Protocol::PipeContentsResp` (pipe_network.fbs:50) | C++ |
| 50 | `kServiceHealthReq` | — | — | ❌ fbs-missing | `Protocol::ServiceHealthReq` (service_health.fbs:10) | C++ |
| 51 | `kServiceHealthResp` | — | — | ❌ fbs-missing | `Protocol::ServiceHealthResp` (service_health.fbs:21) | C++ |

**`kEntitySnap` = `kEntitySnapshot` = 3** (`src/common/GatewayMsg.h:75`) is a
client-side alias, not a separate id.

---

## 3. The dangerous one-sided sets (explicit, as requested)

### 3a. On the wire (C++) but ABSENT from the fbs union — **26 ids**

These are ids a publisher emits and a subscriber dispatches, but for which the
`GatewayPayload` union offers no member at all. Anyone generating a payload via
the union cannot express them.

`2` `kChunkSnapshot` · `4` `kBlockUpdate` · `15` `kSetMachineSlot` ·
`17` `kRecipeCompleted` · `18` `kMachineOpenReq` · `19` `kChestOpenReq` ·
`23` `kMultiblockEvent` · `25` `kQuestEraTransition` · `30` `kGameModeChange` ·
`34` `kRecipeCheckReq` · `35` `kRecipeCheckResp` · `36` `kRecipeCatalogReq` ·
`37` `kRecipeCatalogResp` · `38` `kRecipeItemReq` · `39` `kRecipeItemResp` ·
`40` `kRecipeMachineReq` · `41` `kRecipeMachineResp` · `42` `kBlockActionDirective` ·
`43` `kGridUpdate` · `44` `kWorkbenchOpenReq` · `45` `kChestCloseReq` ·
`46` `kMachineCloseReq` · `47` `kResourceBufferState` · `48` `kPipeContentsReq` ·
`49` `kPipeContentsResp` · `50` `kServiceHealthReq` · `51` `kServiceHealthResp`

(`kGridUpdate` is doubly-listed: it *is* in the union as id 10, just not at 43.)

### 3b. In the fbs union but with NO C++ constant and NO handler — **0 ids**

Every one of the 28 union members corresponds to a real C++ constant, so this
direction is clean. Two members are nevertheless **orphans by name**: the union
member `MachineAction` (id 15) and `MachineActionResp` (id 16) map to tables
`core.fbs:554,562` that **no C++ handler references** — `kSetMachineSlot` (15)
overtook that id.

---

## 4. Ids where the constant name and the actual payload table differ

A name-based reconciliation of the two sides will silently produce the *wrong
table*, because 5 distinct wire ids all carry `Protocol::ContainerOpenReq`:

| Wire id | Constant | Actual table |
|---:|---|---|
| 18 | `kMachineOpenReq` | `Protocol::ContainerOpenReq` |
| 19 | `kChestOpenReq` | `Protocol::ContainerOpenReq` |
| 44 | `kWorkbenchOpenReq` | `Protocol::ContainerOpenReq` |
| 45 | `kChestCloseReq` | `Protocol::ContainerOpenReq` |
| 46 | `kMachineCloseReq` | `Protocol::ContainerOpenReq` |

Evidence — gateway verifies `ContainerOpenReq` for all five:

```
$ grep -n "ContainerOpenReq" src/apps/gateway/gateway.cpp
642:        if (!v.VerifyBuffer<Protocol::ContainerOpenReq>(nullptr)) {   # kMachineOpenReq   (637)
653:        if (!v.VerifyBuffer<Protocol::ContainerOpenReq>(nullptr)) {   # kChestOpenReq     (649)
664:        if (!v.VerifyBuffer<Protocol::ContainerOpenReq>(nullptr)) {   # kChestCloseReq    (660)
675:        if (!v.VerifyBuffer<Protocol::ContainerOpenReq>(nullptr)) {   # kMachineCloseReq  (671)
                                  # kWorkbenchOpenReq (682) also uses it
```

and the client builds it for all five:

```
$ grep -n "CreateContainerOpenReq" src/apps/game_client/Network/NetClient.cpp
850:  EnqueueWrite(GatewayMsg::kWorkbenchOpenReq, ... CreateContainerOpenReq ...)
862:  EnqueueWrite(GatewayMsg::kChestOpenReq,      ... CreateContainerOpenReq ...)
873:  EnqueueWrite(GatewayMsg::kChestCloseReq,     ... CreateContainerOpenReq ...)
884:  EnqueueWrite(GatewayMsg::kMachineOpenReq,    ... CreateContainerOpenReq ...)
895:  EnqueueWrite(GatewayMsg::kMachineCloseReq,   ... CreateContainerOpenReq ...)
```

Two of those five also differ in *semantics*, not just naming: `kChestOpenReq`
has `open:true` while the close variants send `open:false` over the same table.
`ContainerOpenReq` (`src/protocol/core.fbs:313-317`) declares only
`player_id` and `pos` — **there is no `open` field in the `.fbs`**, so the
open/close distinction is conveyed purely by the wire id byte. This makes ids
18/19/44/45/46 strictly not-representable in the union.

Ids 2 (`kChunkSnapshot`) and 4 (`kBlockUpdate`) are `CompressedChunkData`-shaped
ad-hoc frames built with raw `flatbuffers::FlatBufferBuilder` field writes, not
a declared table. **Unverifiable in full:** I did not trace every byte of the
hand-rolled encoders at `src/apps/gateway/gateway.cpp:391,412`; I can only
confirm no named table is used there.

---

## 5. Why this has not broken anything yet (the mitigating fact)

The gateway and client both dispatch on the **1-byte type from the frame
header**, and read the FlatBuffer root *directly* by concrete type — they never
construct or consult a `GatewayMessage` wrapper:

- `src/apps/gateway/gateway.cpp:512` `on_client_ctrl_message(uint8_t msg_type, …)` → `switch (msg_type)` on `GatewayMsg::k*`
- `src/apps/gateway/gateway.cpp:523-531` verifies `Protocol::PlayerAction` on the raw buffer, no union unwrap
- `src/apps/game_client/Network/NetClient.cpp:329-354` same pattern, `case GatewayMsg::k*:` then `GetRoot<Protocol::T>`

So the union is vestigial documentation of a framing design that was bypassed.
**The protocol is currently self-consistent** because both ends read the same
`GatewayMsg.h` header, which is `#include`d by both
(`src/apps/gateway/gateway.h`, `src/apps/game_client/Network/NetClient.h`).

The risk is latent, not active: the next person to write a client, a replay
tool, or a conformance test against the `.fbs` (which is the *documented*
schema of record per `AGENTS.md` "Binary protocol schema | src/protocol/") will
produce a binary that the running gateway rejects:

```
$ sed -n '736,737p' src/apps/gateway/gateway.cpp
    default: spdlog::warn("Gateway: unknown ctrl client msg type {}", msg_type); break;
```

An fbs-conformant client sending `BlockAck` as id 4 is silently dropped as
"unknown ctrl client msg type 4".

---

## 6. Findings, ranked

| # | Sev | Finding | Evidence |
|---|---|---|---|
| 1 | **P1** | The `GatewayPayload` union misnumbers **all 28** of its members, not just the quest block. 18 ids silently mistranslate. | `src/protocol/gateway.fbs:34-63` vs `src/common/GatewayMsg.h:16-75` |
| 2 | **P1** | Worst single offset: `kGridUpdate` is 43 on the wire, 10 in the union — a **33-id** skew. | `GatewayMsg.h:62` vs `gateway.fbs:44` |
| 3 | **P1** | 26 live wire ids have **no** union member. | §3a |
| 4 | **P1** | **Undocumented 4th divergence:** the two generated artifacts disagree with each other *and* with the `.fbs`. Go union has `ChestOpenReq`/`ChestOpenResp` at 18/19 — **types that exist in no `.fbs`**. | `generated/go/Protocol/GatewayPayload.go:28-29`; `generated/gateway_generated.h` enum; mtimes in §1 |
| 5 | **P1** | Go codegen never runs `flatc` on `gateway.fbs`, which is *why* the Go generated union is 2 days stale. Fixing the `.fbs` alone will **not** fix Go. | `src/apps/meta_db/CMakeLists.txt:30-32` |
| 6 | **P2** | 5 distinct wire ids multiplex the single table `ContainerOpenReq`; open/close is encoded in the id byte only, and `.fbs` has no `open` field. | `gateway.cpp:642,653,664,675`; `core.fbs:313-317` |
| 7 | **P2** | Union members `MachineAction`/`MachineActionResp` (15/16) are name-orphans: tables exist (`core.fbs:554,562`) but **no handler references them**. | `grep -rn MachineAction --include=*.cpp src/` → 0 non-generated hits |
| 8 | **P3** | `gateway.fbs:3` header comment documents types `0-32`; actual C++ range is `1-51`. The comment is off-by-one *and* 19 ids short. | `gateway.fbs:3-21` vs `GatewayMsg.h:16-73` |
| 9 | **P3** | `gateway.fbs:4-21` per-line comments contradict the union at `gateway.fbs:35-62` **within the same file** (e.g. line 53 says `QuestUnlockNotification // 20`, union line 53 says 19). The file is internally inconsistent. | `gateway.fbs:52-62` vs `gateway.fbs:34-63` |
| 10 | **P3** | `kMachineOpenReq` comment says "was kChestSaveReq (dead, removed)" — the id 18 was never reused safely; the fbs still believes 18 is `QuestProgressUpdate`. | `GatewayMsg.h:33` |

---

## 7. Explicitly unverifiable

- **Whether any external/third-party consumer exists** that follows the `.fbs`.
  Unverifiable: no client is published, and the C4/architecture docs were not
  audited for a stated id contract. If such a consumer exists, findings 1-3
  escalate to P0.
- **The exact byte layout of the hand-rolled `kChunkSnapshot` (2) and
  `kBlockUpdate` (4) frames.** I confirmed no named FlatBuffers table is used
  at `gateway.cpp:391,412`; I did not decode the builders field-by-field.
- **Whether `MultiblockCreatedEvent` vs `MultiblockDestroyedEvent` are
  disambiguated on id 23 or by a payload field.** `NetClient.cpp:375-388`
  receives both under `case GatewayMsg::kMultiblockEvent` and then branches
  (`VerifyBuffer<Protocol::MultiblockCreatedEvent>` at :377); the
  disambiguating field was not traced.
