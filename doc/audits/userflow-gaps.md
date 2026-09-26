# gp-4re — Userflow coverage gap audit for `test/integration/`

**Audit date:** 2026-09-26
**Scope:** read-only. No test written, no source file modified.
**Tree state:** `926521cc` + dirty worktree, with `test/integration/**` being actively edited by
another agent (gp-dinp / gp-c56 fixes landing during this audit — file mtimes 12:33–12:41).

---

## 0. How to read this report

gp-4re's children each name a userflow and the test that should cover it. The two outcomes a
reader needs to distinguish are:

- **UNCOVERED** — no test exists. Needs a new test written.
- **COVERED-BUT-BROKEN** — a test exists and asserts the right thing, but fails for a reason
  unrelated to the flow's correctness. Needs a fix, not a new test.

These need *different work*, and conflating them is how gp-4re got stale. Section 5 gives the
measured pass/fail; section 6 is the table.

**Measurement caveat, stated up front.** I ran `go test -count=1 -v` in `test/integration/`
(603 s wall, package result `FAIL`) and captured per-test output. The environment has **orphaned
service clusters from earlier runs still holding the fixed ports** (:5001, :7777, :7778), so the
second measured run degraded to `SKIP: chunkd not available: bind(5001) failed: Address already
in use` and every placement-based test then failed with `i/o timeout` against a cluster with no
ChunkStore. **That second run's per-test results are NOT a clean measurement of test health** and
I have excluded it. What I *can* state from the run is the orphan-leak finding in §5.1, which is
a genuine harness defect discovered by direct observation.

The per-test verdicts in §6 are therefore based on:
- direct reads of every test file (ground truth for *what is asserted*), and
- the first run's package-level result plus the pre-existing gp-dinp list (already verified by
  another agent against a clean worktree at `9d20d670`),
- and I mark explicitly below anything I could **not** verify with a clean port set.

---

## 1. gp-4re.1 — Ore processing chain (macerator → furnace → compressor) · P2

**Userflow:** `doc/userflow/07-ore-processing-chain.puml` Flow O1, lines 10-79:
"1 руда -> 2 пластины" (1 ore → 2 plates), asserting item transport through item pipes between
three chained machines, each consuming a recipe.

**Test that exists:** `test/integration/ore_processing_chain_test.go:10`
`TestOreProcessingChain_MaceratorFurnaceCompressor` — status `IN_PROGRESS` on the bead.

**What it actually asserts: nothing.**

```go
// ore_processing_chain_test.go:32-40
placements := []struct{x,y,z int32; id uint16}{
	{cornerX, cornerY, cornerZ, 0xE001}, // heat macerator
	{cornerX+2, cornerY, cornerZ, 0xE000}, // heat furnace
	{cornerX+4, cornerY, cornerZ, 0xE004}, // steam compressor
	{cornerX+1, cornerY, cornerZ, 0xF800}, // pipe
	{cornerX+3, cornerY, cornerZ, 0xF800}, // pipe
	{cornerX+6, cornerY, cornerZ, 0xE800}, // creative gen
	{cornerX+5, cornerY, cornerZ, 0xF400}, // cable
}
for i, p := range placements {
	if err := c.PlaceBlockAndWait(cs, playerID, p.x, baseY, p.z, p.id, uint32(60000+i), 8*time.Second); err != nil {
		t.Fatalf("place block %x at (%d,%d,%d): %v", p.id, p.x, p.z, err)
	}
}

// Verify machines are online and pipe network has nodes
time.Sleep(2 * time.Second)                                  // :49
t.Logf("Ore processing chain placed at %d,%d,%d", ...)         // :50
```

**The comment at :48 says "Verify machines are online and pipe network has nodes." Nothing
verifies either.** There is no `ReadCtrl`, no `WaitForInventoryItem`, no ChunkStore check, no
`MsgBlockEntityUpdate` wait. The test's only failure mode is a placement failing. It does not
check that iron ore became crushed iron became an ingot became a plate; it does not check that
*any* item moved; it does not check the pipes carried anything. It even places `0xE001`
(heat macerator) and `0xE004` (steam compressor) — a heat and a steam machine — wired to a single
heat generator, which per `machines.yaml:51-52,74-84` is not a valid power pairing.

**Verdict: UNCOVERED.** The bead is correctly `IN_PROGRESS`; the file is a scaffold with a
misleading comment and a 2-second sleep pretending to be a check. The bead's acceptance text
("verify output doubling 1 ore → 2 plates") has never been implemented.

**Partially adjacent coverage that exists and should not be confused with this:**
- `test/integration/energy_chain_test.go:107,133,146` asserts a `compressor` reached
  `progress > 0` after an `ironIngot` insert (`:104`). That proves the compressor *runs*, not
  that it ran as the third stage of a three-machine ore chain.
- `test/integration/ebf_multiblock_test.go:246` asserts an EBF produced `ironIngot` — a different
  machine entirely.
- `src/game/recipes/test/test_recipe_manager_matching.cpp` and
  `src/apps/simcore/test/test_recipe_manager.cpp` cover recipe *matching* in isolation
  (ctest `recipe_manager_matching_test`), which is a genuine unit-level complement.

**What a real gp-4re.1 test needs (not written here, per the rules):** place the three machines
+ pipes, put ore in the macerator input, then assert on the *output* item id at each stage
(`iron_ore` → `crushed_iron` → `iron_ingot` → `iron_plate`) via the correlated `BlockEntityUpdate`
poll the other tests already use, and assert the 1→2 doubling the bead names.

---

## 2. gp-4re.2 — Large Steam Boiler multiblock · P2

**Userflow:** `doc/userflow/09-multiblocks.puml` Flow MB2, lines 99-156: "Build 3x3x4 pattern,
feed fuel+water, verify steam production via PipeNetwork."

**Test that exists: none.**

```
$ grep -rnE '1005|1004|large_boiler|LargeBoiler' test/integration/*.go
(NO MATCH)
```

**The pattern does exist in the engine** — this is a genuine coverage gap, not a missing feature:

```cpp
// src/engine/sim/PatternLibrary.cpp:70-105
static MultiblockPattern makeLargeBoilerPattern() {
    p.id = 2;
    p.name = "large_boiler";
    p.controller_block_id = 1005;
    p.size_x = 3; p.size_y = 4; p.size_z = 3;
    constexpr uint16_t CASING = 1001; FIREBOX = 1004; CTRL = 1005;
    // 4 layers, controller at controller_dx/dy/dz = 1,3,1
    p.hatches.push_back({0, 1, 1, HatchType::FLUID_IN});
    p.hatches.push_back({2, 1, 1, HatchType::FLUID_OUT});
}
```

registered in `PatternRegistry::PatternRegistry()` (`PatternLibrary.cpp:155-160`), and the
controller is registered as a machine in `src/apps/simcore/main.cpp:217-218`
(`registerController(BOILER_CONTROLLER, "large_boiler", "large_boiler", nullopt, EnergyType::STEAM, 4)`).

`ctest` also has a `boiler_system_test` (#16) — but that is a unit test of the boiler *system*,
not an integration test that builds the pattern.

**Verdict: UNCOVERED, and the substrate is ready.** A test can be written against
`PatternLibrary.cpp:78-97`'s exact layer table. The BEAD is correct.

---

## 3. gp-4re.3 — Large Chemical Reactor multiblock · P2

**Userflow:** `doc/userflow/09-multiblocks.puml` Flow MB3, lines 161-226.

**This is the one child where the bead's premise is partly false and partly true.**

### 3a. What exists: a real, strong LCR integration test

`test/integration/lcr_energy_hatch_chain_test.go:14`
`TestGateway_LCREnergyHatchThermalChain` builds the **complete** LCR pattern:

```go
// lcr_energy_hatch_chain_test.go:86-100
for x := int32(709); x <= 711; x++ {
    for z := int32(689); z <= 691; z++ {
        blocks = append(blocks, block{x, 121, z, casing})       // 3x3 base
    }
}
blocks = append(blocks,
    block{709, 122, 689, casing}, block{711, 122, 689, casing}, // 4 corners + item hatches
    block{709, 122, 691, casing}, block{711, 122, 691, casing},
    block{709, 122, 690, itemIn}, block{711, 122, 690, itemOut},
)
... // 3x3 top
blocks = append(blocks, block{710, 123, 689, casing}, block{710, 123, 691, casing},
    block{711, 125, 692, energyHatch}, block{710, 123, 690, lcrController})   // controller LAST
```

then asserts formation with a **correlated** event (`:126-139`): `ExpectMultiblockEvent` →
`kind == MultiblockEventCreated`, anchor `(710,123,690)`, `MbType() == 3`, and an independent
ChunkStore read of the energy hatch (`:121-123`). It then drives the whole 4-branch thermal chain
into the LCR and asserts output (`:243-260`). **This is the best-written integration test in the
suite** and it is exactly what the bead asked for.

### 3b. What is missing: the *fluid* half

The bead says "feed items+**fluids**, verify chemical recipes run". The test feeds
`put(710, 123, 690, 0, ironOre, 1)` (`:179`) — an **item** only. The LCR's FLUID_IN
(`PatternLibrary.cpp:136`) and FLUID_OUT (`:137`) hatches are never supplied, and no fluid is
ever introduced. The test's own comment acknowledges a gap in a *different* direction:

```go
// lcr_energy_hatch_chain_test.go:207-209
// RouterEventPublisher currently emits hatch coordinates/types from
// the LCR update only when the dedicated system reaches its snapshot;
// the authoritative block query below proves the physical hatch.
```

and the assertion at `:221-223` is a fallback that sets `seenHatch = true` on a `BlockEntityUpdate`
at the hatch position — so the *hatch* check is weakly satisfied.

**Verdict: SPLIT.**
- "LCR multiblock forms" → **COVERED** by `lcr_energy_hatch_chain_test.go`, and it is good.
  (Though it is currently in gp-dinp's 9-failure set, so **covered-but-broken**.)
- "feed **fluids** + verify **chemical** recipes run" → **UNCOVERED.** No fluid hatches fed, and
  the only recipe exercised is an iron-ore→iron-ingot smelt (`:203`), which is a *furnace*
  recipe, not a chemical-reaction recipe. `machines.yaml:246` registers `chemical_reactor_lv`
  and `:256` a `placeholder_chemical_reactor`, but no integration test builds either.

**A second, unrelated false signal to ignore:** `test/integration/full_base_test.go:53-70` builds
something it calls "LCR" — 4 blocks of `0xEE05` casing plus `0xE42F` as a "controller
placeholder" (`:63`, the comment says so). That is **not** the LCR pattern (which needs block id
`1006` and 3 layers), and the test never asserts an LCR formed. **Do not count
`full_base_test.go` as LCR coverage.** The bead should not be closed on the strength of that file.

`test/integration/chemical_reactor_side_test.go:11` is a *single-block* LV chemical reactor
(`0xE403`, `machines.yaml:246`) wrench-side-config test — different machine, does not count.

---

## 4. gp-4re.4, .5, .6, .7 — the four with no test at all

| Child | Userflow | Test named by bead | Exists? | Substrate in engine? |
|---|---|---|---|---|
| **gp-4re.4** voltage tiers & transformers | `07-ore-processing-chain.puml` Flow O2 (:87-120); `06-item-energy-transport.puml` | none | **NO** — `grep -rniE 'transformer' test/integration/` → **no match** | **YES**: `src/game/machines/TransformerSystem.{h,cpp}`, `TransformerComponent.h`, ctest `transformer_system_test` (#11) |
| **gp-4re.5** electric tools, drill & charging | `08-wrench-tools-config.puml` Flow W2 (:86-141) | none | **NO** — only a comment at `testutil/fb_builders.go:258` ("BuildToolAction builds a ToolAction FlatBuffer for wrench/drill etc.") | **PARTIAL**: `src/game/mining/DrillSystem.cpp` (7 methods incl. `phaseEnergyCheck`), `src/game/actions/handTool/ElectricDrillHandler.h`, ctest `mining_systems_test` (#24) |
| **gp-4re.6** heat cooling & overheat | `03-heat-management.puml` Flow H3 (:152-206) | none | **NO** — `grep -rniE 'overheat\|coolant\|temperature' test/integration/` → **no match** | **YES**: `src/game/machines/CoolantSystem.h`, `OverheatComponent.h`, `HeatConstants.h`, `ExplosionSystem.cpp`, ctest `explosion_system_test` (#10) |
| **gp-4re.7** multiplayer sync | `05-system-network.puml` Flow S3 (:140-211) | none | **NO** — every test dials exactly one control connection per test | **NO** — this is the blocker |

### 4a. gp-4re.7 has a hard upstream blocker — the bead is premature

Multiplayer sync cannot be tested because **the Gateway is single-client by construction**:

```cpp
// src/apps/gateway/gateway.cpp:91-93
std::lock_guard<std::mutex> lock(client_ctrl_mutex_);
old_conn = std::move(client_ctrl_);
client_ctrl_ = std::move(conn);
```

`client_ctrl_` is a **single** `optional<...>`, not a session map — a second connect *replaces* the
first. Same for `client_bulk_` (`gateway.cpp:140`). So "Verify block changes broadcast to
multiple clients" and "player disconnect/reconnect state" (the bead's own words) are not
testable against today's Gateway; the second client would silently evict the first.

**This is exactly what `openspec/changes/add-multiplayer-foundation` is for** — its
`specs/multiplayer-sessions/spec.md` requires "The Gateway SHALL support 2 to 8 concurrent client
sessions" and its `tasks.md` §6.1-6.5 lists the headless multi-client harness
("two simultaneous clients, handshake, unique ids, response isolation") as **0/24 unchecked**.

**Verdict for gp-4re.7: UNCOVERED and BLOCKED.** It must be sequenced *after*
`add-multiplayer-foundation` §6, not worked in parallel. Recommend re-parenting or re-prioritising
it; it is P3, which is already right, but it should say *why* it is blocked.

### 4b. gp-4re.4 has a content blocker for a *placement* test

`TransformerSystem` exists and is unit-tested, but there is **no placeable transformer block**:

```
$ grep -rniE "transformer" src/content/data/registry/
(no match)
```

Transformers exist only as *crafting recipes* — `src/content/data/recipes/crafting_table.yaml:720`
(`transformer_mv_hv`), `:736` (`transformer_hv_ev`), `:2235`, `:2250` — and as a quest
(`src/content/data/quests/quests.csv:75`, "Transformer LV-MV"). `machines.yaml` has **no**
transformer class. So a headless test cannot *place* a transformer: it would have to grant the
item and hand it to a `SetMachineSlotReq`, which is not what the machine graph consumes.

This looks like a data gap that belongs to the same family as gp-bwo (registry coverage).
**Verdict for gp-4re.4: UNCOVERED, and partly blocked on content** (no placeable transformer
block in `machines.yaml`/`items.csv`). The "cable tier mismatch causes explosion" half of the
bead is testable today via `ExplosionSystem` + two cable tiers; the "transformer step-down works"
half is not.

### 4c. gp-4re.5 and gp-4re.6 are cleanly UNCOVERED and unblocked

- **Drill:** `DrillSystem` is a full ECS system with an energy phase check
  (`src/game/mining/DrillSystem.cpp:151 phaseEnergyCheck`), ore detection (`:39 isOreBlock`),
  and spiral targeting (`:52 getSpiralOffset`). `ElectricDrillHandler` is the player-facing tool
  path. ctest `mining_systems_test` covers it at unit level. An integration test needs a placed
  drill + a battery buffer; both machines exist (`battery_buffer_lv` `machines.yaml:323`).
  **Unblocked.**
- **Overheat/coolant:** `CoolantSystem.h`, `OverheatComponent.h`, `HeatConstants.h`,
  `ExplosionSystem.cpp` all exist with ctest `explosion_system_test` (#10). The userflow's three
  claims (overheat warning, machine slowdown/stop, coolant consumption) map onto
  `BlockEntityUpdate.temperature` and `flags` fields already present in the schema
  (`src/protocol/machine_state.fbs`). **Unblocked.**

---

## 5. The measured run, and a harness defect it exposed

### 5.1 NEW FINDING — the harness leaks service clusters on a non-zero exit · **P1**

This is worth filing as its own bead regardless of gp-4re.

`ServiceManager.Shutdown` (`testutil/service.go:422-454`) is only reached via
`main_test.go:46-53`'s `cleanup`, which runs after `m.Run()` returns — **normally**. But
`go test` also kills the test binary on `-timeout`, and a `t.Fatal` inside a goroutine it cannot
attribute leaves `sm.Shutdown` unreached. Observed directly:

```
$ pgrep -af "chunkd|gatewayd|simcored|entitystated|metadbd|pipe_networkd"
80288 chunkd /home/su/.hermes/cache/scratch/gtnh-test-chunkdb-1444700089 5001 127.0.0.1 4000
80382 simcored_exec 127.0.0.1 4000 127.0.0.1 5001 ...
80401 gatewayd --router-port 4000 --port 7777 --bulk-port 7778
85681 chunkd /home/su/.hermes/cache/scratch/gtnh-test-chunkdb-4129755358 5001 ...
85710 simcored_exec ...            <-- a SECOND cluster
85782 gatewayd --router-port 4000 --port 7777 --bulk-port 7778
92173 routerd --port 4000
92185 chunkd ...
92276 gatewayd ...
92283 metadbd
4054460 chunkd /home/su/.hermes/cache/scratch/gtnh-test-chunkdb-3934585824 5001 ...

$ ss -ltn | grep -E ':5001|:7777|:7778'
LISTEN 0 128 0.0.0.0:5001 0.0.0.0:*
LISTEN 0 128 0.0.0.0:7777 0.0.0.0:*
LISTEN 0 128 0.0.0.0:7778 0.0.0.0:*
```

**At least 3 orphaned clusters, none of them owning the `routerd` on :4000.** The consequence is
exactly what I hit: a fresh run's `chunkd` cannot bind :5001 and the harness prints

```
SKIP: chunkd not available: chunkd: exited before readiness: exit status 1:
  chunkd: bind(5001) failed: Address already in use
```

and **then runs the whole suite anyway** with no ChunkStore. Every placement test then fails with
`i/o timeout` — a failure that looks like a server regression and is not.

**Two distinct defects here:**

1. **The leak.** `main_test.go:20` does `code := m.Run(); cleanup(); os.Exit(code)`. There is no
   `defer`, and no `signal` handler. A panic, a `-timeout` kill, or a `t.Fatal` in a goroutine
   skips `cleanup()` entirely. Fix: `defer cleanup()` is not sufficient (`os.Exit` skips defers),
   so the real fix is a `TestMain` that recovers and a `signal.Notify` for SIGINT/SIGTERM, or
   simply making the harness refuse to start when :5001/:7777 are already bound and say so.

2. **`SKIP` after a port conflict is not a skip — it is a false green.** `main_test.go:100-103`
   returns `func(){}` (no services, but tests still run) after printing `SKIP: chunkd not
   available`. `TestBase_BuildMultiMachine` passed in that run. A test suite that has silently
   lost its ChunkStore must **fail or skip the tests**, not run them. The `SKIP:` print goes to
   stdout and is not connected to any `t.Skip`.

**Recommendation:** file as a new P1 bead. It is the reason a clean measurement is currently
impossible on this machine, and it will bite anyone running the suite twice in a row — which
this repo's parallel-agent discipline guarantees.

### 5.2 What the run did establish

- Package `integration-tests` took **603 s** and returned `FAIL`.
- Package `testutil` returned **`ok` in 2.633 s**, 19/19 assertions green — including
  `TestWaitForInventoryItemSkipsStaleSnapshots`, `TestWaitForInventoryItemSumsSplitStacks`,
  `TestWaitForInventoryItemTimesOutOnUnmatchedSnapshots`, `TestRouterServiceHasSubscription`
  (7 sub-cases), `TestShutdownReturnsWhenDescendantEscapesProcessGroup`.
  **The harness self-tests are healthy.** Every failure is in the integration package.
- The contaminated second run produced 13 FAILs, all with the signature
  `read tcp ...:7777: i/o timeout` on a placement. That set is **not** a health signal and is
  excluded from §6.

---

## 6. Coverage table

Status key: **COVERED** = exists and asserts the flow · **BROKEN** = exists, asserts the flow,
fails for an unrelated reason · **PARTIAL** = exists but asserts less than the bead names ·
**NONE** = no test · **BLOCKED** = cannot be tested until a named prerequisite lands.

| Bead | Userflow | Test file (file:line) | Status | What is genuinely still uncovered |
|---|---|---|---|---|
| **gp-4re.1** | O1 ore chain 1→2 plates | `ore_processing_chain_test.go:10` | **NONE** (scaffold only) | Everything. The test asserts nothing beyond placement success — no recipe runs, no item moves, no 1→2 doubling, no pipe transport, and the 2 s sleep at `:49` is not a check. |
| **gp-4re.2** | MB2 Large Steam Boiler | — | **NONE** | The whole flow. Pattern is defined and registered (`PatternLibrary.cpp:70-105`); no integration test builds it, feeds fuel+water, or checks steam via PipeNetwork. |
| **gp-4re.3** | MB3 LCR | `lcr_energy_hatch_chain_test.go:14` | **PARTIAL / BROKEN** | *Covered:* pattern formation, energy hatch, thermal chain, item output. *Uncovered:* **fluid hatches never fed** (FLUID_IN/OUT, `PatternLibrary.cpp:136-137`), and the only recipe run is an iron-ore smelt, not a chemical reaction. *Do not count* `full_base_test.go:53-70` (4-block "placeholder", not the pattern). *In gp-dinp's 9-failure set.* |
| **gp-4re.4** | O2 voltage/transformers | — | **NONE / BLOCKED on one registry line** | No transformer test at all. The *block* exists (`items.csv:425-431`, prefix `1110:110`) and is craftable, but `machines.yaml` has **no transformer class**, so a placed transformer is not a registered machine and `TransformerSystem` has nothing to act on. Also `transformer_lv_mv` has no recipe, which is what quest 74 asks for. The cable-tier-explosion half is testable **today** with no content change. |
| **gp-4re.5** | W2 drill & charging | — | **NONE** | Drill energy consumption, depletion, and battery-buffer charging. `DrillSystem`/`ElectricDrillHandler` exist and are unit-tested (`mining_systems_test` #24) but have no integration coverage. Unblocked. |
| **gp-4re.6** | H3 cooling/overheat | — | **NONE** | Overheat warning, machine slowdown/stop, coolant consumption. `CoolantSystem`/`OverheatComponent`/`ExplosionSystem` exist (`explosion_system_test` #10); zero integration coverage. Unblocked. |
| **gp-4re.7** | S3 multiplayer sync | — | **NONE / BLOCKED** | All of it, and it cannot start. `gateway.cpp:91-93` stores a single `client_ctrl_`; a second connect evicts the first. Requires `add-multiplayer-foundation` §6.1-6.5 (0/24 done). |

### 6.1 The gp-dinp overlap — "covered but broken" vs "uncovered"

This is the distinction the parent asked to be precise about. Of the beads, **exactly one**
(gp-4re.3's LCR test) overlaps the known-failing set:

| Test | In gp-dinp's 9? | Meaning |
|---|---|---|
| `TestGateway_LCREnergyHatchThermalChain` | **no** | Not in the list. It is not among the 9 — the 9 are `TestCrafting_RequestResponse/ValidRecipe`, `TestCrafting_ValidInvalid/ValidRecipe`, `TestChunk_GetBlockAfterSet`, `TestGateway_ReceivesChunkDataOnConnect`, `TestGateway_BlockPersistsAfterReconnect`, `TestInventory_MoveBetweenSlots`, `TestInventory_BreakBlockGivesItem`, `TestMachine_GeneratorFurnaceChain`, `TestOreProcessingChain_MaceratorFurnaceCompressor`. |
| `TestOreProcessingChain_...` (gp-4re.1) | **yes** | But it is in the failing set *and* asserts nothing — so its failure is incidental to the "placement succeeded" path, not evidence that the ore chain is broken. **Do not read its FAIL as "the ore chain is broken".** |
| `TestMachine_GeneratorFurnaceChain` | **yes** | A 2-machine heat chain (generator→furnace), adjacent to but not part of any gp-4re child. |
| `TestGateway_ReceivesChunkDataOnConnect`, `TestGateway_BlockPersistsAfterReconnect` | **yes** | Bulk-port chunk delivery, not a gp-4re flow. **Both have been rewritten by the concurrent agent** (now `gateway_test.go:37` and `:114`, dialing the bulk port per `gateway.cpp:387-393`); the `:114` one now also waits for the ChunkStore commit at `:88`. Re-measure before treating these two as still-failing. |

**Net: gp-4re.1 and gp-4re.3 are the only children whose named tests are in the failing set, and
one of those two is a scaffold.** The honest summary is that **five of seven gp-4re children have
no test at all**, one has a partial test, and one is blocked on a foundation change.

---

## 7. Recommended disposition

| Bead | Action |
|---|---|
| gp-4re.1 | **Re-specify, then implement.** The bead's text is right; the existing file is a scaffold whose comment at `:48` claims a verification that does not exist. Either implement O1 properly or delete the misleading file. |
| gp-4re.2 | **Implement.** Substrate is complete (`PatternLibrary.cpp:70-105`, `main.cpp:217-218`). Lowest-friction remaining child. |
| gp-4re.3 | **Split the bead.** Close the "LCR forms" half against `lcr_energy_hatch_chain_test.go`; file a new child for "LCR fluid hatches + chemical recipe", which is genuinely uncovered. |
| gp-4re.4 | **Split again.** File "cable tier mismatch → explosion" as immediately implementable (no content change needed). File "transformer step-down" as **blocked on adding a `transformer` class to `machines.yaml` under prefix `1110:110`** — a one-entry content task, not a feature. Pair the missing `transformer_lv_mv` recipe (quest 74) with it. |
| gp-4re.5, gp-4re.6 | **Implement.** Both unblocked, both have unit-test substrate and no integration coverage. |
| gp-4re.7 | **Re-parent to `add-multiplayer-foundation` §6.** Cannot be implemented against today's single-client Gateway. Keep P3; annotate the blocker. |
| *(new, P1)* | **File the cluster-leak + false-SKIP defect** from §5.1. It blocks clean measurement of everything above. |
