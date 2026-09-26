# Audit gp-ukwn — `ROADMAP.md` gaps: architectural work in flight but absent from the roadmap

**Date:** 2026-09-26 · **Commit:** `926521cc` · **Scope:** read-only. `ROADMAP.md` is NOT modified.

**ROADMAP.md location:** repo root, `ROADMAP.md` (705 lines, mtime 2026-09-19 — same
week as `AGENTS.md`). There is no `doc/ROADMAP.md`.

Method: read all of `ROADMAP.md`, then diff its content against
`openspec/changes/*/tasks.md` (14 live changes) and `bd list --status open` (128 open
issues). Every item below cites the ROADMAP line it is missing from **or** contradicting,
plus the live artefact that proves the work exists.

For each item the question is answered as one of:
- **SILENT** — the roadmap does not mention this work at all.
- **CONTRADICTS** — the roadmap states something that the code/artefacts falsify.
- **STALE** — the roadmap tracks the work but its numbers/paths are out of date.

---

## 0. Two framing problems, before the gaps

### RG-00 (CRITICAL) — `ROADMAP.md:573-592` ("Work tracking") is the authoritative-work index, and it is wrong on 6 of 9 entries

`ROADMAP.md:577-588` prints a tree of in-flight openspec changes with progress counts.
Compared against the real directories and the real checkbox counts:

| `ROADMAP.md:579-587` claims | Reality | Status |
|---|---|---|
| `add-chunk-interest-aggregator/ # WIP (0/20)` | `0/20` | ✅ |
| `add-headless-multiblock-gateway-tests/ # WIP (2/12)` | dir is now `add-headless-multiblock-gateway-tests-**verification**`; tasks `13/13` | **STALE + renamed** |
| `add-interaction-mode-gating/ # WIP (0/5)` | `1/5` | **STALE** |
| `add-service-health-debug-window/ # WIP (8/12)` | **shipped and archived** → `openspec/changes/archive/2026-09-21-add-service-health-debug-window` | **STALE (completed, unrecorded)** |
| `complete-autonomous-mining/ # WIP (0/9)` | `0/9` | ✅ |
| `fix-ore-processing-chain/ # WIP (0/26)` | `9/26` | **STALE** |
| `questbook-client-polish/ # WIP (4/10)` | `4/10` | ✅ |
| `refactor-item-registry-hierarchy/ # WIP (8/20)` | `9/20` | **STALE** |
| `refactor-server-authoritative-inventory/ # WIP (28/34) — почти готов` | dir is now `refactor-server-authoritative-inventory-**verification**`; tasks `0/6`; the parent change is **gone** | **STALE + renamed** |

And **7 live changes are missing from the list entirely**:

```
add-headless-thermal-power-chain-verification                        0/4
add-multiplayer-foundation                                           0/24
add-pipe-fluid-overlay                                               0/9
add-scaled-energy-hatch-multiblock-verification                     0/5
refactor-fluid-port-accounting                                       0/30
refactor-server-authoritative-inventory-verification                 0/6
separate-hbf-ebf-blast-furnaces-verification                         0/5
```

Plus `add-chunk-interest-aggregator`, whose 20 tasks are the largest single gateway
change in flight.

**Impact:** `ROADMAP.md:575` asserts "Active work is tracked as openspec changes". A
reader following that pointer gets 9 of 14, with 3 counts wrong, 2 entries pointing at
renamed directories, and 1 entry (`add-service-health-debug-window`) that **shipped and
was archived** while the roadmap still shows it as 8/12 WIP. The roadmap's only view of
in-flight work is unreliable. Everything in §1 below is partly a consequence of this.

### RG-00b (HIGH) — a `-verification` suffix wave has renamed half the live changes, and the roadmap does not model it

`ls openspec/changes/` shows **7 of 14** live changes carry a `-verification` suffix that
did not exist a week ago:

```
add-headless-multiblock-gateway-tests-verification
add-headless-thermal-power-chain-verification
add-scaled-energy-hatch-multiblock-verification
refactor-server-authoritative-inventory-verification
separate-hbf-ebf-blast-furnaces-verification
```

…whereas the *original* changes they verify (`add-headless-multiblock-gateway-tests`,
`refactor-server-authoritative-inventory`) are **gone from the live tree entirely** —
they moved to `openspec/changes/archive/`, and the verification change is the continuation.

This is a structural convention the roadmap has never recorded, and it is the reason
three of the RG-00 rows are wrong. `ROADMAP.md:590-592` lists only the archive's
*contents*, never its *convention*. A reader looking for
`add-headless-multiblock-gateway-tests/` will not find it and may conclude the work
was abandoned rather than renamed and continued.

Worth noting for whoever documents this: the same 7-change wave is what puts a
**spec delta** in `openspec/specs/` for work the roadmap believes is still in progress
(e.g. `openspec/specs/thermal-power-chain/`, `openspec/specs/pipe-fluid-overlay/`,
`openspec/specs/service-health/` — see 1.4, 1.5, 1.7).

### RG-01 (HIGH) — the roadmap still speaks the pre-rename `src/services/*` layout in 11 places

`src/services` does not exist (`ls -d src/services` → no such directory). Occurrences:

`ROADMAP.md:36, 206, 274, 625, 626, 627, 628, 629, 630, 631, 664`

The worst block is the **run order**, `ROADMAP.md:624-636`, which tells the reader to
launch 8 binaries at paths that do not exist:

```
./cmake-build-debug/src/services/message_router/routerd        # :625
./cmake-build-debug/src/services/chunk_store/chunkd            # :626
./cmake-build-debug/src/services/entity_state_store/entitystated # :627
./cmake-build-debug/src/services/gateway/gatewayd              # :628
./cmake-build-debug/src/services/simulation_core/simcored_exec # :629
./src/services/meta_db/metadbd                                  # :630
./cmake-build-debug/src/services/pipe_network/pipenetworkd     # :631
```

Compare `README.md:150-159`, which has the same list with the **correct** `src/apps/*`
paths (and 2 wrong binary names — see `stale-docs.md`). The roadmap is strictly worse
than the README here.

`ROADMAP.md:603` ("Симуляция / ECS / мультиблоки → `simulation_core/`") and
`ROADMAP.md:607` ("`services/recipe_manager/`") are stale in the same way.
`AGENTS.md:172` explicitly warns that "`README.md` and `run.sh` still reference the old
`src/services/*` layout" — but it does not mention that **`ROADMAP.md` does too, and
more extensively**.

### RG-02 (HIGH) — `ROADMAP.md:42` says "FlatBuffers схемы (12 файлов)"; there are 14

`ls src/protocol/*.fbs | wc -l` → **14**. The table at `ROADMAP.md:44-57` lists 12 rows
and omits `client_state.fbs` and `service_health.fbs` — both of which are real and both
of which are actively generated (`gateway/CMakeLists.txt:13,15` lists
`client_state_generated.h` and `service_health_generated.h` as flatc outputs, and
`GatewayMsg.h:72-73` defines `kServiceHealthReq/Resp = 50/51` off the back of them).

`ROADMAP.md:598` repeats the wrong count in the "Где что лежит" table
("`src/protocol/*.fbs` (12 файлов)"). `AGENTS.md:56` and `README.md:197` both say 14
and are right.

---

## 1. Architectural work in flight, absent from the roadmap

### 1.1 Fluid port accounting refactor — SILENT, and the largest untracked change in the repo

`openspec/changes/refactor-fluid-port-accounting/` — **0/30 tasks**. Zero references in
`ROADMAP.md` (grep: none). Zero beads issues (grep: none).

**Important nuance found on close inspection:** an *earlier* version of this change was
already archived as `openspec/changes/archive/2026-09-01-refactor-fluid-port-accounting`
(43 entries in `archive/`). Comparing the two proposals, the live one is a **reopened
and materially expanded** version, not a duplicate:

- archived version and live version share the same "Why" (typed resource ports, the
  boiler HEAT-sink/STEAM-source identity collision, the registry ID-domain split)
- the live version adds a clause the archived one lacks: *"Ports do not introduce
  resource filters in this change; concrete FLUID/ITEM operations carry their actual
  resource IDs"* — i.e. the scope was deliberately narrowed/clarified on reopen
- the live version drops the registry-migration language that the archived "Why" had
  (`pipes.csv` / `cables.csv` / `fluids.csv` mixing canonical item references with
  independent small integer IDs), moving that to a **separate** live change,
  `refactor-item-registry-hierarchy` (see 1.12)

So the roadmap is silent on a refactor that has already been attempted once, archived,
reopened, and resized — and that is coupled to a second live refactor. Neither attempt
nor reopen is recorded anywhere in `ROADMAP.md`.

The roadmap's nearest neighbours are `ROADMAP.md:453-465` (backlog E "Энергетика и
трубы") and `ROADMAP.md:557-567` (M "Глубокие симуляции" — which lists "Жидкости:
смешивание, вязкость" as a *deferred L* idea). Neither mentions the refactor.

The roadmap currently claims the fluid story is done: `ROADMAP.md:176-200` "Этап 3:
PipeNetwork ✅" and `ROADMAP.md:196-200` "Осталось" is a short list that does not
include it. **This is the single largest silent gap.**

### 1.2 Pipe fluid overlay (client) — CONTRADICTS (roadmap calls it archived-and-done; a reopened 0/9 version is live)

`ROADMAP.md:590-592` lists `pipe-fluid-overlay` among the **archived** changes
("Архив: `openspec/changes/archive/` (включая 09-12: … pipe-fluid-overlay, …)").

Reality: `openspec/changes/add-pipe-fluid-overlay/` is **live with 0/9 tasks**, and a
live spec `openspec/specs/pipe-fluid-overlay/` exists. Comparing the archived
(`archive/2026-09-12-add-pipe-fluid-overlay`) and live proposals, the live one is
**reopened and reduced**: the archived version required authoritative buffer values
(*"live buffer values MUST come from the server-authoritative resource-state channel;
the client must not query PipeNetwork directly"*), while the live version drops that and
describes a **client-only** change (*"This is a **client-only** change (game_client
service). No protocol, server, or simulation changes."*), scoped to the topology mask
via `PipeMeshBuilder::detectConnections` plus the existing `WrenchOverlay`.

So the requirement was *narrowed on reopen*, and the roadmap — which says the whole
thing is archived — is describing neither the archived scope nor the live one.

Also note the live proposal at `openspec/changes/add-pipe-fluid-overlay/proposal.md`
still refers to `src/services/game_client/` in its Impact section — so even the live
openspec artefacts are partly on the old layout, which is the likely source of the
roadmap's confusion.

Tracker presence is thin: `gp-e7u` (P2, "Render square beveled procedural transport
pipes") is adjacent but is a different task. `ROADMAP.md:506-524` is backlog I
"Клиент: UI/рендер/ощущения [S/M]" — the natural home — but the
`ROADMAP.md:640-655` "Известные проблемы" table is silent, and the archive listing at
`:590` actively misleads.

### 1.3 Blast furnace separation — CONTRADICTS, not SILENT (amended after checking backlog D)

`openspec/changes/separate-hbf-ebf-blast-furnaces-verification/` — 0/5 tasks.
No `ROADMAP.md` mention by name. The only tracker presence is two P2 beads:
`gp-kmhv` ("resolve coil aggregation and muffler/hatch presence rules") and
`gp-o1mw` ("specify removal cleanup of furnace endpoints and pending requests").

I checked `ROADMAP.md:434-451` (backlog D "Машины и мультиблоки L4+") for a home: it
lists only *new* content (hull tiers, more furnaces, oil processing, assembly line,
covers, EU tiers) — **nothing about separating the two furnace types that already have
content**. And `ROADMAP.md:151` marks Stage 2 (Multiblocks L2+L3) as ✅ **done**.

Meanwhile the tree has `EBFSystem` registered (`src/apps/simcore/main.cpp:107` region)
*and* a separate `hbf.yaml` in `src/content/data/recipes/` — two furnace tiers of
content served by one system, with a live change proposing to split them and a second
proposing to verify the split. **The roadmap asserts a ✅ that an unstarted change is
explicitly trying to invalidate.** That is a contradiction, not a silence.

### 1.4 Scaled energy hatch multiblock — SILENT

`openspec/changes/add-scaled-energy-hatch-multiblock-verification/` — 0/5 tasks.
Backed by 3 open P2 beads: `gp-475t` (connect cable endpoint to hatch position),
`gp-mjmo` (refactor the thermal fixture into reusable helpers), `gp-0a17` (state-transition
assertions for steam/EU/batteries/32-EU).

`ROADMAP.md:211` (Stage 2) claims "**Multiblocks L2+L3**: … item IO, block-break guard,
persistence, client GUI, FlowHandlers" is ✅ done. But three of the five verification
tasks for the energy-hatch multiblock are open, and `gp-475t` is a *positioning* bug —
the hatch's cable endpoint is not connected to the hatch position. The roadmap's
"✅ multiblocks done" claim is ahead of the tree.

### 1.5 Thermal power chain verification — SILENT

`openspec/changes/add-headless-thermal-power-chain-verification/` — 0/4 tasks,
plus `gp-0a17` (P2) directly under it.

`ROADMAP.md:209` (Stage 1) and `ROADMAP.md:176-195` (Stage 3) both present heat/steam/
energy as delivered: "✅ **Heat/Boiler**: HeatTransferSystem — 6-neighbor propagation,
overheat detection (90%/100%)" is `README.md:209`; `ROADMAP.md:196-200` "Осталось" for
PipeNetwork does not list thermal verification.

There is a dedicated live spec `openspec/specs/thermal-power-chain/` and 4
unstarted verification tasks against it. The roadmap has no section for it.

### 1.6 Multiplayer foundation — CONTRADICTS (the roadmap calls it "proposal ready", not "0/24 tasks in flight")

`ROADMAP.md:393` — "## A.1. Мультиплеер-фундамент [M] — ПЕРЕД B (openspec
`add-multiplayer-foundation`, **proposal готов**)".
`ROADMAP.md:652` — "Gateway single-client … 🟡 PROPOSAL".

Reality: `openspec/changes/add-multiplayer-foundation/` exists with **24 tasks, 0 done**.
`openspec/changes/add-chunk-interest-aggregator/` (0/20) is the spec'd follow-on
(`gp-k3w` P3: "spec the wire messages before implementing them"). A live spec
`openspec/specs/multiplayer-sync/` exists. `doc/c4/level4-deployment.puml:48,66`
already draws the "PLANNED A.1" fanout.

So: 24 tasks of work exist and the roadmap describes the state as a finished proposal.
The **intent** is captured (and it is the right priority — "ПЕРЕД B"); the **state** is
wrong by a wide margin. This is the most important CONTRADICTS in the roadmap because
it governs the ordering of backlog B (network stack v2) and H (entities/multiplayer).

### 1.7 Service health / Router client health probes — CONTRADICTS

`ROADMAP.md:528` (backlog J) — "Health checks: конкурентные probes (**beads gp-otl**) + /health REST".

Reality: `bd show gp-otl` → `◐ IN_PROGRESS`, `[BUG] Make all Router clients answer health probes`,
P2, **lease expired** (heartbeat 2026-09-14, i.e. 12 days stale). A live spec
`openspec/specs/service-health/` exists, and `GatewayMsg.h:72-73` carries
`kServiceHealthReq/kServiceHealthResp = 50/51` — so the protocol is landed, the router
side exists (`router.go:188-189` `health`, `healthProbes` maps), and the bug is that
**not all clients answer**.

The roadmap files this as a backlog-J nice-to-have. It is actually a half-landed feature
with a stalled owner. The roadmap also no longer mentions the
`add-service-health-debug-window` change that `ROADMAP.md:582` used to track
(see RG-00) — the client-side health window appears to have shipped (there are
`gameclient_service_health_store_test` binaries in `cmake-build-debug/bin/`) without the
roadmap recording it.

### 1.8 Engine/sim layer separation — CONTRADICTS (roadmap says "do this now", tree says the inversion is now a P2 bug with 3 open issues)

`ROADMAP.md:331-369` is "Этап 11: Modding & Layer Separation 🔴", with
`ROADMAP.md:352` "## Что делать сейчас (не рантайм, а шов — общий для обеих веток)" and
11 unchecked items.

Reality — there is a live spec `openspec/specs/engine-layer-separation/` and **3 open
P2 beads**:
- `gp-9j0` "Stage 11: Engine layer separation (static lib split)"
- `gp-50p` "engine/sim SimulationEngine still wires game/machines systems (**layering inversion**)"
- `gp-1c74` "Verify the engine/sim layering inversion inventory (gp-50p / gp-9j0) without refactoring"

`src/apps/simcore/CMakeLists.txt:161` shows the inversion concretely — a
`$<LINK_GROUP:RESCAN,...>` with the comment "Circular static deps: game/machines calls
back into simcored (Network clients, RecipeManager) while simcored calls machine
systems."

So the roadmap's Этап 11 framing ("modding foundation, deferred until the first mod")
**understates** the problem: the layering inversion is *already present and already
broken*, is a confirmed bug, and has 3 open tickets. The roadmap's Q14
(`ROADMAP.md:699`) defers the *mod runtime* (.so vs WASM) — correctly — but the
roadmap never separates "deferred mod runtime" from "actively broken engine layering".
`doc/c4/README.md:106` gets this right ("Layer separation: src/engine/, src/game/,
src/content/, src/apps/ (refactor-engine-layer-separation)") — the C4 README knows
the refactor is real; the roadmap does not.

### 1.9 Chunk interest aggregator / gateway interest management — STALE (tracked, but understated and the spec task itself is a P3)

`ROADMAP.md:136-141` "### 1.1 Gateway interest management 🟡" and
`ROADMAP.md:579` "add-chunk-interest-aggregator/ # 🟡 WIP (0/20)".

Reality: 0/20 tasks, and the *first* task — designing the wire messages — is itself an
unstarted P3 bead, `gp-k3w` "add-chunk-interest-aggregator: **spec the wire messages
before implementing them**". Related open P2: `gp-0yf` "Document or implement the
gateway interest-management **placeholder**".

The roadmap presents this as 🟡 WIP mid-flight. It is 0/20 with its first step
unstarted. The C4 diagrams (`level3-gateway.puml`) do not show an interest-management
component at all, so the diagram is silent too (see `c4-drift.md` C4-10).

### 1.10 Autonomous mining completion — STALE (roadmap says done in two places, change is 0/9)

`ROADMAP.md:250-259` is "Этап 7: Будущее ⏸". `ROADMAP.md:583` says
`complete-autonomous-mining/ # 🟡 WIP (0/9) — drill persistence, UI`.

Backed by 4 open P2 beads: `gp-1kdy` (drill output buffer → item pipe),
`gp-j8gc` (restore drill state on SimCore restart), `gp-vmat` (drill state save/load via
EntityStateStore RPC), `gp-wso6` (integration test: electric tools).

And 3 P1 bugs in the same subsystem: `gp-xotc` "DrillSystem issues only TWO block
requests ever: pendingSearches_ is reassigned, not decremented", `gp-4v5i`
"getSpiralOffset never returns the origin … search 8-directional", `gp-7n7s` "DrillSystem
derives the mined drop from the CAS reply, not the targeted ore, so drop-less ore can
wedge the output buffer".

`ROADMAP.md:207-208` in `README.md`'s status list reads "✅ **Autonomous Mining**:
DrillSystem — spiral BFS ore search…" — and a drill that issues only two block requests
ever, searches 8 directions, and can wedge on drop-less ore is not done. The roadmap
does not carry a "drill is not actually autonomous yet" line anywhere.

### 1.11 Interaction mode gating — STALE

`ROADMAP.md:581` "add-interaction-mode-gating/ # 🟡 WIP (0/5)" → real `1/5`.
Open beads: `gp-5cr` (P3, "make the 1.1 predicate a named helper"),
`gp-ul16` (P1, "**BUG** the game-mode interaction gate is a **deny-list**, so it admits
every mode the enum does not define").

A P1 correctness bug in a gate that admits undefined enum values is not represented in
`ROADMAP.md:640-655` "Известные проблемы".

### 1.12 Item registry hierarchy refactor — STALE

`ROADMAP.md:586` says `8/20`, real `9/20`. Backed by 5 open beads: `gp-o6u8` (CLI
subcommands), `gp-c2u` (P3, "resolve task 1.1 **blocking decision** on namespaces"),
`gp-eux` (replace the Category enum placeholder), `gp-4bh` (registry ItemId packing vs
items.csv), `gp-a4k`/`gp-yt2x` (item registry model tests failing on the real registry).

`gp-c2u` says task 1.1 is **blocked on a decision**. `ROADMAP.md:362` lists
"Namespaces модов" as an unchecked Этап-11 item — so the roadmap has the *topic* but
does not record that a live 20-task refactor is stalled behind it. A reader would
conclude the refactor is progressing.

### 1.13 Protocol wire-constant divergence — the roadmap is *more* current than AGENTS.md, and should be credited

`ROADMAP.md:644` — "GatewayMsg C++ константы vs FlatBuffers `GatewayPayload` union —
расхождение | 🔴 TODO (backlog A)".
`ROADMAP.md:645` — "Мёртвые сообщения GridUpdate / MachineAction / MachineActionResp | 🔴 TODO (backlog A)".

Reality: **5 open P3 beads** now track this granularly — `gp-a6l` (GatewayMsg.h vs
gateway.fbs divergence), `gp-ocjg` (mark the stale union explicitly), `gp-48he`
(every .fbs table used by a publisher but absent from GatewayMsg.h), `gp-k8ap`
(document the 1-based numbering rule in the header), `gp-3dw6` (verify Go builder call
order). Plus `openspec/changes/questbook-client-polish/` has a task to correct the
numbering (`gp-5gi`).

**This is correct and current.** Verified further: `src/protocol/gateway.fbs:34-54` — the
union stops at `QuestCompletedNotification = 21` and **skips 17 and 18**, while
`src/common/GatewayMsg.h:73` reaches 51. The roadmap's characterisation ("устарел",
"мёртвые члены") is accurate.

### 1.14 MB_ID write path — CONTRADICTS (roadmap says "🔴 TODO", open beads say the opposite direction)

`ROADMAP.md:653` — "**mb_id write path отсутствует** в ChunkStore (meta-layer пишется,
путь не полный) | 🔴 TODO".

Reality: `src/apps/chunk_store/Storage/cache/MutableChunk.h:57-59` has
`getMultiblock`/`setMultiblock` on `MutableChunk`, and
`Storage/cache/MutableSection.h:43,88-89` has `mb_entries` / `setMultiblock` on the
section, with the whole sparse→flat machinery at `:40-45`. The write path exists at
the data-structure level.

What is actually broken is different: **`MultiblockController` is never emplaced as an
ECS component**, which is why the *consumers* never fire. That is open ticket
`gp-qgtc` (P1) — "ExplosionSystem/AdjacencyTransferSystem/CoolantSystem can never fire -
MultiblockController is never an ECS component" — and `gp-wjwr` (P1) "AdjacencyTransferSystem:
pass-2 overheat view is dead (MultiblockController never emplaced)".

So the roadmap names the wrong broken thing, at the wrong layer, and misses two **P1**
bugs that are the actual root cause. This is worse than staleness: a fixer reading
`ROADMAP.md:653` would go implement a write path that already exists and leave the P1s.

### 1.15 MessageRouter delivery guarantees — CONTRADICTS with the code

`ROADMAP.md:654` — "**MessageRouter: нет at-least-once ack+retry, нет no-self-delivery**
(SHALL-gaps) | 🔴 TODO (backlog B)".

Reality: `src/apps/message_router/router.go:185-188` uses
`subs map[string]map[*client]struct{}` and `:187` `connServices map[*client]string` —
a per-connection service name is tracked, which is the *mechanism* for no-self-delivery.
Whether the delivery loop actually consults `connServices` needs a read of the fan-out
function; the roadmap asserts it is absent. **Not resolved in this audit** — flagged as
needs-verification rather than asserted as a finding, but it is a load-bearing claim in
"Известные проблемы" that no one has re-checked in 7 days.

### 1.16 Quest registry gap — STALE number, correctly tracked

`ROADMAP.md:655` — "Quest registry gap — **102/158** целей нет в items.csv | 🔴 TODO
(beads gp-bwo)".

The bead `gp-bwo` is still open (`bd list --status open` shows it, P2). The specific
ratio 102/158 has not been re-verified in this audit. Correctly tracked; the count is
probably stale but the item is live. Low action.

### 1.17 Chunk storage format migration — SILENT, and it contradicts the roadmap's own bandwidth math

The roadmap carries the 192 KB chunk figure as a **bandwidth trigger** at
`ROADMAP.md`-adjacent `doc/archive/init_adr.analysis-techdebt.md:29` ("Day 72 — 192KB/chunk
× 10 chunks × 10 players = 19.2 MB/s"). The actual format is palette-encoded at
**~260 bytes** (`src/apps/chunk_store/Storage/chunkd_load_test.cpp:96`; format spec at
`Storage/SectionCodec.h:5-15`, `SEC_SZ=16/SEC_VOL=4096/SEC_CNT=8` at `:20-22`).

No roadmap section covers the migration of this assumption, and nothing records that
the bandwidth gate which justified several deferred items **no longer applies**. Related
open P2: `gp-4fy` "Fix io_uring write batch limit TODO **with its stated prerequisite**"
— a prerequisite that is now obsolete.

### 1.18 Test-coverage architecture — SILENT as a theme

`ROADMAP.md:525-539` (backlog J) lists contract tests generically
("Contract-тесты для всех RPC-границ"). But the tracker now holds a **coordinated
test-hardening programme** the roadmap has no section for:

- `gp-4re` + 6 children (`gp-4re.2`–`gp-4re.7`) — integration test suite per userflow,
  each split out as its own ticket
- `gp-iwa8`, `gp-oi3b`, `gp-qct5`, `gp-wso6` — the "already specified, split per-test" work
- `gp-ama3` (P3) "Add a ctest label for the integration tests so they can be selected
  separately" — i.e. integration tests are not currently selectable
- `gp-vu1t`/`gp-vu1` (P3) "Audit: test/integration harness services that still rely on
  fixed sleeps"
- `gp-dinp` (P1) "**9 integration tests fail on current main** — PRE-EXISTING, not a regression"
- `gp-2rl`, `gp-3ah`, `gp-2df` — three P3 audit tickets
- `gp-zx4f` (P3) "Verify the full ctest suite is green and record the current baseline"
- `gp-xxb` (P3) "Write the missing engine/sim unit-test target (openspec 2.4 deviation)"

**A P1 ticket says 9 integration tests are red on main, and `ROADMAP.md` — whose entire
status vocabulary is ✅/🟡/🔴 — does not mention it.** The roadmap's Stage 1 "Стабилизация
геймплея ✅" and the ✅ marks on Stages 2/3/8/9/10 are not reconcilable with a red suite.

### 1.19 Graphify / codegraph tooling as a project dependency — SILENT

`gp-0851` (P3) "Make the graphify graph update a documented step rather than tribal
knowledge". `AGENTS.md` has a whole `## graphify` section mandating
`graphify query` before code navigation and `graphify update .` after edits, plus a
`codegraph_explore` MCP daemon. `ROADMAP.md` has **no mention of graphify, codegraph, or
`.codegraph/`** anywhere in 705 lines. The repo has `graphify-out/` with 15 subdirectories
and `.codegraph/` committed.

For a roadmap that is the "where are we going" document, the tooling every agent is
required to use is absent from it.

### 1.20 Go sidecar and LMDB error-handling audits — SILENT as a quality theme

Eight P3 audit tickets define a recognised quality backlog the roadmap does not carry:
`gp-4ll` (ignored-error return sites in C++), `gp-7d1` (Go sidecars with no error
wrapping at process boundaries), `gp-8lc` (LMDB chunk store read/write error
swallowing), `gp-ejc` ((void)-cast unused-parameter sites), `gp-gkv` (dead code in
`game/ui` and `game/client`), `gp-npvm` (go vet baseline), `gp-g4d`/`gp-weyz`
(documentation of build/test commands and the private-netns run recipe).

`ROADMAP.md:525-539` (backlog J) covers packaging, health checks, metrics, logs, load
tests, benchmarks, fuzzing, crash reports, migrations, CI, C4, contract tests — but
**not** static analysis, error handling, dead code, or developer-onboarding docs. Seven
P1 bugs in the tracker (`gp-4ll` family aside) are in fact error-handling bugs:
`gp-frb2` (falls through into the FIFO branch, one response credits two machines),
`gp-u9ua` (node_id 0 sentinel collides with entt's first entity id),
`gp-paja` (NaN satisfies every condition), `gp-dyo4` (hash collision: N identical stacks
== empty inventory).

### 1.21 Layer/ownership conventions for parallel agents — SILENT

`gp-z9tj` (P3) "Record the parallel-agent file-ownership rules as a repo convention".
`AGENTS.md` has a whole `## Agent Toolchain` section with "Parallel agents: OpenCode
agents in `.claude/worktrees/` may commit to `main` during your session… Run
`git pull --rebase` before touching shared zones: `src/protocol/`, `src/content/data/registry/`,
`src/content/data/recipes/`, `CMakeLists.txt`, `conanfile.txt`". None of that is in
`ROADMAP.md`, which has no "how we work" section at all.

---

## 2. What the roadmap gets RIGHT (do not rewrite these)

A gap audit that only lists gaps misleads. Verified correct:

1. **`ROADMAP.md:30`** — SpatialIndex: "СТАБ — `main.cpp` = 2 строки, `add_subdirectory`
   закомментирован, не собирается". Exactly right (`src/apps/spatial_index/main.cpp` is
   2 lines; `CMakeLists.txt:84` commented). ✅
2. **`ROADMAP.md:37`** — Validation: "**НЕ в корневом CMakeLists** — не собирается по
   умолчанию". Right, and stronger than the wording implies: there is no
   `add_subdirectory(src/apps/validation…)` line at all, not even commented. ✅
3. **`ROADMAP.md:27`** — WorldGenerator: "**Библиотека, не сервис** (линкуется в chunkd)".
   Exactly right, and **`doc/c4/level2-container.puml:33,89-90,103` contradicts it** —
   see `c4-drift.md` C4-03. The roadmap is the accurate document here. ✅
4. **`ROADMAP.md:26`** — ChunkStore "palette-native MutableChunk" and the 2026-07-31
   libgtnh-net rewrite. Right (`Storage/cache/MutableChunk.h`, `IoUringChunkStoreService.cpp`). ✅
5. **`ROADMAP.md:47,60`** — the `GatewayPayload` union is stale; GridUpdate /
   MachineAction / MachineActionResp are dead. Right (see RG 1.13). ✅
6. **`ROADMAP.md:59`** — "`item_registry.fbs` **НЕ СУЩЕСТВУЕТ** — item registry =
   `data/registry/items.csv` + items.db". Right — `ls src/protocol/` has no
   `item_registry.fbs`, and `src/content/data/registry/items.csv` + `items.db` both exist. ✅
7. **`ROADMAP.md:648`** — "SpatialIndex не собирается (`add_subdirectory` закомментирован)
   | `CMakeLists.txt` | 🔴 TODO (этап 4, backlog K)". Right. ✅
8. **`ROADMAP.md:613`** — "Никогда не пересобирать с нуля, не удалять
   `cmake-build-debug/` / `cmake-build-release/` — внутри Conan toolchain". Right, and it
   matches `AGENTS.md:129-131`. ✅
9. **`ROADMAP.md:616-618`** — the build command `cd cmake-build-debug && ninja -j5` works. ✅
10. **`ROADMAP.md:531-539` (backlog J)** — the infra/quality list is well-chosen and
    still relevant; it is only *incomplete* (see 1.18-1.20). ✅
11. **The stage structure itself (0→11 plus A–M backlog)** is a sound organising scheme
    and maps well onto the tree. The problem is maintenance, not design. ✅

---

## 3. Grouped summary of what a follow-up task should add

| Group | Items | Roadmap status | Suggested home |
|---|---|---|---|
| **G1 — Live changes absent from the roadmap** | `refactor-fluid-port-accounting` (0/30, reopened after a 09-01 archive), `add-scaled-energy-hatch-multiblock` (0/5), `add-headless-thermal-power-chain` (0/4) | SILENT ×3 | New stage 12, or fold into Stage 2 (multiblocks) + Stage 3 (pipes) |
| **G1b — Roadmap asserts a ✅ a live change contradicts** | `separate-hbf-ebf-blast-furnaces` (0/5) vs `ROADMAP.md:151` Stage 2 ✅; `add-pipe-fluid-overlay` (0/9) vs `ROADMAP.md:590` archive listing | CONTRADICTS ×2 | Same, plus correct the two ✅/archive claims |
| **G2 — Wrong state on tracked work** | `add-multiplayer-foundation` (0/24 vs "proposal ready"), `add-chunk-interest-aggregator` (0/20, first task a P3), `complete-autonomous-mining` (0/9 + 3 P1 bugs), `add-interaction-mode-gating` (1/5 + 1 P1 deny-list bug), `refactor-item-registry-hierarchy` (9/20, blocked on a decision) | CONTRADICTS ×2, STALE ×3 | Fix RG-00 (regenerate the work-tracking tree) + correct each count |
| **G3 — Wrong diagnosis** | `mb_id write path` (RG 1.14) — the data structure exists; the real bug is 2× P1 `MultiblockController never emplaced`; `MessageRouter no-self-delivery` (1.15) — unverified, contradicted by `connServices` | CONTRADICTS | `ROADMAP.md:640-655` "Известные проблемы" — rewrite, add the 2 P1s |
| **G4 — Quality programme with no home** | 9-red-integration-tests P1 (`gp-dinp`), ctest labelling, ctest baseline, 3 audit clusters (ignored errors, Go wrapping, LMDB swallowing, dead code, go vet), 4 one-line doc tickets | SILENT | Extend backlog J with subsections J.1 (test baseline) and J.2 (static analysis / error handling) |
| **G5 — Tooling & conventions** | graphify/codegraph as a mandated step, parallel-agent file ownership | SILENT | New "Как мы работаем" section, parallel to the existing "Архитектурные заметки" |
| **G6 — Obsolete premise** | 192 KB chunk figure as a bandwidth trigger; io_uring batch-limit "stated prerequisite" | CONTRADICTS (the premise is now wrong) | `ROADMAP.md:525-539` + the deferred-item justifications |
| **G7 — Path/count rot** | 11× `src/services/*` incl. the whole run-order block; 12 vs 14 `.fbs` (twice); `ROADMAP.md:598` "12 файлов"; `:603` `simulation_core/`; `:607` `services/recipe_manager/`; `:599-601` `gateway/message_router_client.*` (no such file — it is `gtnh::net::RouterClient` in `src/engine/net/`) | STALE | A single mechanical pass, `src/services` → `src/apps`, `simulation_core` → `simcore` |

---

## 4. Verdict

`ROADMAP.md` is **directionally sound, factually stale**. Its architecture decisions
(worldgen-as-library, spatial stub, the `GatewayPayload` verdict, the
never-rebuild-the-Conan-dirs rule) are all verified correct and mostly *better* than
`doc/c4/`. Its **maintenance** is what failed: the work-tracking index at
`ROADMAP.md:577-588` is 6/9 wrong, the run-order block at `:624-636` points at a
directory layout that no longer exists, and 5 live openspec changes — including the
largest refactor in the repo at 30 tasks — have no mention at all.

The most dangerous class is **G3**: where the roadmap names a specific broken thing and
the real defect is a P1 bug elsewhere. `ROADMAP.md:653` is the clearest example and
would actively misdirect a fixer.

Suggested follow-up order: G7 (mechanical, ~30 min, unblocks every reader) → RG-00
(regenerate the work index) → G3 (rewrite the two wrong diagnoses) → G1/G2 (add the
missing stages, correct the counts) → G4/G5 (new sections).
