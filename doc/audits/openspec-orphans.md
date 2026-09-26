# gp-3ah — OpenSpec requirements reachable from an active change with no test and no task

**Audit date:** 2026-09-26
**Scope:** `openspec/changes/**` (14 active changes) → requirement → task → test mapping.
Read-only. No source file modified, no `openspec` state mutated
(`openspec validate <change> --strict` was run for all 14; it is read-only).

---

## 0. Method

1. Enumerate active changes (`ls openspec/changes/ | grep -v '^archive$'` → **14**).
2. For each, parse `specs/*/spec.md` into `### Requirement:` blocks and their `#### Scenario:`s.
3. For each change, parse `tasks.md` checkboxes.
4. Search the tree for a test that exercises the requirement — `ctest -N` (54 targets),
   `test/integration/**`, `src/**/test/`, `tools/test_*.py`.

`openspec validate <change> --strict` passes for **all 14** changes, so every spec delta is
structurally valid; the gap this report is about is *verification*, not validity.

Two open items gp-3ah explicitly told me to verify rather than assume. Both are checked in §4
and §5 — **both premises turned out to be stale.**

---

## 1. The headline: how many requirements are asserted in prose and verified by nothing

| Change | Reqs | Has a test for… | **Orphaned reqs** |
|---|---|---|---|
| `add-chunk-interest-aggregator` | 3 | 0 | **3** |
| `add-headless-multiblock-gateway-tests-verification` | 2 | 2 | 0 |
| `add-headless-thermal-power-chain-verification` | 1 | 1 | 0 |
| `add-interaction-mode-gating` | 1 | 1 (unit) | 0 (integration task open) |
| `add-multiplayer-foundation` | 10 | 0 | **10** |
| `add-pipe-fluid-overlay` | 1 | 0 (test written, never compiled) | **1** |
| `complete-autonomous-mining` | 3 | 0 | **3** |
| `fix-ore-processing-chain` | 5 | 0 | **5** |
| `questbook-client-polish` | 6 | 1 | **5** |
| `refactor-fluid-port-accounting` | 6 | 0 | **6** |
| `refactor-item-registry-hierarchy` | 5 | 3 | **2** (one blocked on missing CLI subcommands) |
| `add-scaled-energy-hatch-multiblock-verification` | 0 (no `specs/`) | — | n/a |
| `refactor-server-authoritative-inventory-verification` | 0 (no `specs/`) | — | n/a |
| `separate-hbf-ebf-blast-furnaces-verification` | 0 (no `specs/`) | — | n/a |

**Totals: 42 requirements across 11 changes that carry a spec delta. 35 have no test that
exercises them. 3 changes (14 tasks between them) have no `specs/` directory at all** — see §3.

**A caveat that applies to the whole table, stated plainly:** a requirement with no test is only
a *defect* if the change is expected to be verifiable now. For 9 of the 11 changes the
implementation has not started (`0/N` tasks checked), so "no test" is the correct state, not an
orphan. The genuinely actionable subset is §2 — requirements that are **already implemented and
shipped** with nothing verifying them, or whose spec prose asserts a hard `SHALL` that no task
even schedules.

---

## 2. The genuinely orphaned requirements — implemented or unbacked

### 2.1 `fix-ore-processing-chain` — 5 requirements, 0 tests, and the code is half-landed

This is the **worst** case in the repo. 9 of 26 tasks are checked, and the spec has 5 `SHALL`
requirements including one that reads as a prohibition:

| Requirement | tasks.md status | Test |
|---|---|---|
| `Item String Names in Recipes` — "Hardcoded numeric item IDs SHALL NOT be used" | 2.1, 3.1, 4.1 ✅ done; **5.1 audit "zero hits expected" ☐ open** | **none** |
| `Complete Ore Processing Recipes` (2.2, 2.4, 3.2, 4.3 all ☐ open) | partial | **none** |
| `Item Registry Coverage` (1.3 ☐ open) | partial | **none** |
| `Recipe Correctness` (5.2 "every string name resolves via `nameToId()`" ☐ open) | partial | **none** |
| `Item Pipe Transport Integration` | 5.6 ☐ open | **none** |

**The critical point:** requirement 1 is a *negative* requirement ("SHALL NOT use numeric IDs")
whose enforcement task 5.1 is **unchecked**, and whose scenario is literally "Numeric ID is
rejected." There is no test anywhere that feeds a numeric `item:` to the parser and asserts
rejection. A regression that reintroduces numeric ids would pass the entire 54-target ctest
suite and the entire Go suite.

**Related, and measured:** `grep -rn "hierarchical\|nameToId" src/game/recipes/test/
src/apps/simcore/test/` matches only `test_recipe_manager.cpp`, `test_quest_manager.cpp` and
`test_game_scenario.cpp` — and `grep -rn "RecipeItemIdFormat|parseItemId|resolveItemName" src/game/
recipes/test/ src/apps/simcore/test/` returns **nothing**. The recipe-id format surface named by
`refactor-fluid-port-accounting`'s `recipe-id-format` delta has **zero unit tests**.

**Actionable:** add a parser unit test covering the three formats
(`refactor-fluid-port-accounting` `recipe-id-format` requires hierarchical / flat-numeric /
string-name) and the "no fallback parsing" requirement, plus a ctest that greps the recipe YAML
corpus for `item: \d+`. Both are small and would close 3 requirements across 2 changes.

### 2.2 `questbook-client-polish` — 5 of 6 requirements unbacked, and the client IS implemented

| Requirement | Implemented? | Test |
|---|---|---|
| `FlatBuffers quest parsing with enum constants` | ✅ `QuestBookWindow.cpp:598-615` uses `GatewayMsg::kQuestUnlockNotification` / `kQuestCompletedNotification` | **none** (no ctest targets `QuestBookWindow`) |
| `Unlock notification` (2.1 ☐ open) | partial | none |
| `Completion notification` (2.2 ☐ open) | partial | none |
| `Era completion badges` (3.1 ☐ open) | ✗ | none |
| `Era lock state` (3.2 ☐ open) | ✗ | none |
| `No client write-path for quest status` | ✅ (tasks 4.1/quest.set marked REMOVED) | **none** |

**Requirement 6 is the one worth acting on.** It is a prohibition — "the client SHALL NOT send
quest progress writes over `meta_db.quest.set`" — and it is the kind of rule that erodes silently.
A one-line test asserting the client registers no `quest.set` route would lock it in. (The client
test target list — `ctest -N` #42-#53 — has drag manager, gamemode, scenario, static assert, mesh
hash, wrench ×4, service health, recipe mirrors, interaction system, gamemode gate. **No quest book
target at all.**)

### 2.3 `add-chunk-interest-aggregator` — 3 requirements, 0 tests, 0/20 tasks, and the service does not exist

Requirements: `Chunk Interest Aggregator`, `Chunk Unload Notification`, `Chunk Ownership Transfer`.

```
$ find src -ipath '*chunk*interest*'                      # -> nothing
$ grep -rn 'ChunkBatchNotify|ChunkUnloadNotify|ChunkInterestRegister' src/protocol/*.fbs   # -> nothing
```

**Correctly orphaned — nothing is implemented and tasks 1.1-5.5 (0/20) schedule the work,
including 5.1-5.5 which are the unit and integration tests.** This is a well-formed change, not a
gap. Listed for completeness only. **No action.**

### 2.4 `add-pipe-fluid-overlay` — 1 requirement, implemented, test written but **NEVER RUNS** · **highest-value finding in this report**

The requirement is client-side UI:

> "The game client SHALL provide a toggleable, client-side overlay that highlights the connected
> faces of the fluid pipe block currently targeted by the player's highlight ray."

**It is implemented:**

```
src/game/ui/client/core/ActionHandler.cpp:39   reg->Register("toggle_pipe_fluid_overlay", ...)
src/game/ui/client/core/InputBinder.cpp:45     Bind(GLFW_KEY_L, "toggle_pipe_fluid_overlay")
src/game/client/GameClient.cpp:542             frd.ext.showPipeFluidOverlay = pipe_fluid_overlay::ShouldShowOverlay(...)
src/game/client/GameClient.cpp:545             if (frd.ext.showPipeFluidOverlay) {
```

**And a test for it is already written — 20,702 bytes, 14 test functions — but it is not compiled:**

```
$ grep -nE 'tests/[A-Za-z_]+\.cpp' src/apps/game_client/CMakeLists.txt
227:        tests/DragManager_test.cpp
246:        tests/test_gamemode_permissions.cpp
261:        tests/test_start_scenario_command.cpp
274:        tests/test_client.cpp
281:        tests/MeshHash_test.cpp
290:        tests/WrenchOverlay_test.cpp
303:        tests/WrenchGrid_test.cpp
315:        tests/WrenchMetaLink_test.cpp
337:        tests/ServiceHealthStore_test.cpp
368:        tests/test_recipe_mirrors.cpp
```

`src/apps/game_client/tests/PipeFluidOverlay_test.cpp` is **absent from that list**, and
`ctest -N | grep -i pipe` returns only `pipe_network_test` (#42) — no overlay target. The 14
functions it defines (`test_gate_toggle_off`, `test_gate_non_fluid_targets`,
`test_gate_fluid_and_dense`, `test_mask_connected_disconnected_faces`, `test_mask_meta_gating`,
`test_state_text_*`, `test_pipe_contents_*`, `test_read_only_guard`) map **one-to-one** onto the
requirement and its four scenarios ("overlay shows connected faces", "inactive when toggle off",
"ignores non-fluid blocks", "no protocol or server impact" → `test_read_only_guard`).

**So this requirement is not "untested" — it is tested by code that never executes.** One
`add_executable` + `add_test` block, copied from the adjacent `WrenchOverlay_test.cpp` entry
(`CMakeLists.txt:290`), turns a written test into a running one. **This is the single cheapest
closure in the whole audit.**

**Two more orphans of the same shape, found by the same grep:**

| Orphaned test file | Size | Wired to a target? |
|---|---|---|
| `src/apps/game_client/tests/PipeFluidOverlay_test.cpp` | 20,702 B | **NO** |
| `src/apps/game_client/tests/test_item_registry.cpp` | 4,698 B | **NO** |

`test_item_registry.cpp` matters for **two** changes at once:
`refactor-item-registry-hierarchy` and `refactor-fluid-port-accounting`'s
`architecture :: Canonical Cross-Registry Item Identity`. Both have a requirement that this
unrun file was evidently written for.

**An inconsistency worth flagging:** the change is 0/9 tasks, yet tasks 2.1, 2.2, 3.1, 3.2 and 4.1
are all implemented in the tree, **and the test is written**. The `tasks.md` is badly out of date
relative to the code — which means `openspec` reports this change as not-started when it is
substantially complete including its test. That drift is exactly what makes an audit like this one
produce false results, and it is the root cause of this finding being invisible.

### 2.5 `complete-autonomous-mining` — 3 requirements, unit tests exist, no integration

| Requirement | tasks | Unit test | Integration |
|---|---|---|---|
| `Autonomous Mining Persistence` (1.1, 1.2 ☐ open) | ☐ | — | — |
| `Item Pipe Integration` (2.1, 2.2 ☐ open) | ☐ | — | — |
| `Drill Client UI` (3.1-3.3 ☐ open) | ☐ | — | — |

Drill machinery exists (`src/game/mining/DrillSystem.cpp`, ctest `mining_systems_test` #24) but
nothing covers the three *specified* behaviours: persistence across restart, output into an item
pipe, and the drill window. This is the same gap as **gp-4re.5** in
`doc/audits/userflow-gaps.md` §4c — the two beads describe the same missing coverage from
different angles. **Recommend merging or cross-linking them** so the work is not done twice.

### 2.6 `refactor-fluid-port-accounting` — 6 requirements, 0 tests, 0/30 tasks, not started

Requirements across `architecture` (3), `pipes-cables-transport` (1), `recipe-id-format` (2).

`PortId` / `ResourceKind` exist only in `src/common/ResourcePort.h:11` (`enum class ResourceKind`)
— a header stub; the 30 tasks that build the port system are all open. **Correctly orphaned.**

The exception is `recipe-id-format`, which is *not* the subject of this change's tasks and has
**no test at all** (see §2.1). That one is a real orphan today.

### 2.7 `refactor-item-registry-hierarchy` — 5 requirements, 3 covered, 2 orphaned

This is the **best-formed** change in the repo and the model for the others: 9/20 tasks done,
`tools/test_item_registry_model.py` exists with 13 `test_` functions, and tasks 4.1-4.3 are
checked off against real tests.

| Requirement | Test |
|---|---|
| `Separate logical groups from allocation prefixes` | ✅ `tools/test_item_registry_model.py` (tasks 4.1-4.3 ✅) |
| `Canonicalize allocation prefixes with a dedicated prefix grammar` | ✅ task 4.1 ✅ (`0:1:0` / `0:10` equivalence) |
| `Calculate capacity from address ranges` | ✅ task 4.2 ✅ |
| `Preserve existing IDs during allocation and merge` | ❌ task 4.4 ☐ open |
| `Validate logical and allocation relationships` | ❌ task 4.4 ☐ open (2.2 is a model test ✅; the strict *validator* CLI path is not) |

Both orphans share one missing thing: **nothing tests `tools/item_registry_cli.py` itself.**
There is no `tools/test_item_registry_cli.py`; `find tools -name 'test*.py'` returns only
`test_item_registry_model.py` and `test_generate_texture_mappings.py`.

**And there is a second, unrecorded orphan:** `src/apps/game_client/tests/test_item_registry.cpp`
(4,698 B) exists and is not wired into any CMake target (see §2.4's table) — so a C++-side
registry test is also written-but-never-run.

**Task 4.4 is the one to action**, and note its scope is currently misstated. `tasks.md` 4.4 says
*"Add CLI tests proving dry-run commands do not write and invalid input returns non-zero"* and
task 3.1 says *"Add CLI subcommands for `validate`, `stats`, `allocate`, and `merge --dry-run`"*.
But the CLI today exposes only three subcommands:

```
$ grep -nE 'add_parser' tools/item_registry_cli.py
190:    validate = sub.add_parser("validate", ...)
195:    stats    = sub.add_parser("stats", ...)
200:    migrate  = sub.add_parser("migrate", ... "--dry-run", action="store_true", ...)
```

`allocate` and `merge` **do not exist yet** (3.1 is unchecked, correctly). So the requirement
`Preserve existing IDs during allocation and merge` has **no implementation to test** — it is
correctly orphaned against unchecked task 3.1, and task 4.4 is blocked behind it.

**The genuinely actionable part** is narrower than it looks: the *validator* half of
`Validate logical and allocation relationships` can be tested today against the existing
`validate` subcommand, without waiting for `allocate`. A test that feeds `validate` a fixture
with overlapping sibling ranges, a duplicate packed id, and an out-of-range payload, and asserts
non-zero exit for each, closes half of requirement 5 today.

### 2.8 `add-multiplayer-foundation` — 10 requirements, 0 tests, and blocked on the Gateway

10 requirements across `multiplayer-sessions` (7) and `multiplayer-sync` (3), 0/24 tasks, and
`ClientSession` does not exist (`grep -rn 'class ClientSession' src/apps/gateway/` → nothing;
`gateway.cpp:91-93` still holds a single `client_ctrl_`).

**Correctly orphaned.** Note that this is the same blocker as **gp-4re.7** —
`doc/audits/userflow-gaps.md` §4a. Tasks 6.1-6.5 *are* the multi-client harness gp-4re.7 wants.
**Recommend gp-4re.7 be closed as a duplicate of `add-multiplayer-foundation` §6**, not worked in
parallel.

---

## 3. The three changes with no `specs/` directory — a structural gap

| Change | Has | tasks.md |
|---|---|---|
| `add-scaled-energy-hatch-multiblock-verification` | `proposal.md`, `tasks.md` | 0/5 |
| `refactor-server-authoritative-inventory-verification` | `proposal.md`, `tasks.md` | 0/6 |
| `separate-hbf-ebf-blast-furnaces-verification` | `proposal.md`, `tasks.md` | 0/5 |

```
$ for d in openspec/changes/*/; do ... ls ...; done
add-scaled-energy-hatch-multiblock-verification              proposal.md tasks.md
refactor-server-authoritative-inventory-verification           proposal.md tasks.md
separate-hbf-ebf-blast-furnaces-verification                  proposal.md tasks.md
```

**These are requirements that are asserted in prose but have no spec and therefore cannot be
validated at all** — `openspec validate <change> --strict` passes vacuously for all three,
because there is no delta to check. This is exactly gp-3ah's "asserted in prose but verified by
nothing," one level up from the requirement.

**What the prose claims, and whether anything checks it:**

**`separate-hbf-ebf-blast-furnaces-verification`** (0/5). Tasks 4.1 and 4.3 are the test tasks:
- 4.1 *"Add focused unit tests for EBF EU and HBF HU execution, stalls, and exact outputs"* ☐
- 4.3 *"Keep direct thermal and LCR energy-hatch regressions green"* ☐

The ctest suite has `ebf_system_test` (#14) but **no `hbf_system_test`** in the 54-target list.
And `grep -rn '"hbf"' src/apps/simcore/` matches only `main.cpp:215,217,231` (registration) and
`test_ecs_systems.cpp:935` (`hbf.machine_class = "hbf"` — a field assignment, not an execution
test). `PatternLibrary.cpp:150-152` does register the pattern, and the game client mirrors it at
`GameClient.cpp:255-257`. **So: the HBF pattern is registered in three places and executed by no
test.** A fourth `hbf_system_test` target next to `ebf_system_test` closes it.

**`refactor-server-authoritative-inventory-verification`** (0/6). Its tasks are almost entirely
*deletions* (5.2 delete `RenderSlotGrid`, 5.3 delete `MachineSlotHandler`). Two of those are
already done and appear in today's `git log`: `5e1e1c18 refactor(ui): drop RenderSlotGrid, which
has no call site` and `88d63621 test(client): cover the gamemode gate, container inventory and
block drops`. **The tasks.md is stale relative to the tree here too** — same drift as
`add-pipe-fluid-overlay` (§2.4). Task 5.5 is the only real verification gate and it is open.

**`add-scaled-energy-hatch-multiblock-verification`** (0/5). Task 1.3 is the test task:
*"Add focused tests for LCR ENERGY hatch detection, absent hatch rejection/fallback, tier, and
face configuration"* — ☐ open, and `grep -rn "lcr" test/integration/` finds only
`lcr_energy_hatch_chain_test.go` and the `full_base_test.go` placeholder, neither of which tests
absent-hatch rejection or face configuration. **Orphaned, and the change has no spec to validate
the requirement against.**

---

## 4. gp-bwo verification — **the premise is now largely FALSE**

gp-3ah asked me to verify, not assume, that gp-bwo's "category C" quests (items absent from
`items.csv`) are still uncompletable. I checked directly rather than reading the bead.

**Every hierarchical item id referenced by every quest now resolves in `items.csv`:**

```python
# 163 quests parsed from src/content/data/quests/quest_requirements.json
# 384 items parsed from src/content/data/registry/items.csv
# quests referencing a hierarchical id NOT in items.csv: 0
```

Spot-checks against gp-bwo's own list:
- Q44 → `1110:111:0` → present
- Q113 → `0:110:27` → present
- Q153 → `0:110:29` → present

**Going one step further — do the referenced items have recipes?** Counting every `item:` and
`- name:` across all of `src/content/data/recipes/*.yaml`:

```
requirements with NO recipe referencing the item: 2
    ('37',  'craft', '1110:100:0',  'creative_generator')
    ('74',  'craft', '1110:110:2',  'transformer_lv_mv')
```

**gp-bwo claimed 62 uncompletable quests. There are 2, and both are explainable:**
- **Q37** requires *crafting* `creative_generator` — a machine you are given in creative mode,
  not crafted. This is a quest-data modelling error, not a missing item.
- **Q74** requires *crafting* `transformer_lv_mv`. The transformer exists in the registry, but
  `crafting_table.yaml` has recipes for `transformer_mv_hv` (`:720`), `transformer_hv_ev` (`:736`),
  `transformer_ev_iv` (`:2235`) and `transformer_iv_luv` (`:2250`) — **not** the LV→MV one the
  quest asks for. `quests.csv:75` ("Transformer LV-MV, Craft a transformer to step up LV to MV")
  and the quest requirement disagree with the recipe file. A real, small, fixable data bug.

**This also independently confirms gp-4re.4's content blocker** (`doc/audits/userflow-gaps.md`
§4b), and pins the fix precisely. Transformers *are* registered as placeable block ids —
`items.csv:425-431` allocates prefix `1110:110` to `transformer_mv_hv` / `transformer_hv_ev` /
`transformer_lv_mv` / `transformer_ev_iv` / `transformer_iv_luv`, under a comment reading
`# machines/transformers additions — quest category C`. So the item is there and placeable; what
is missing is the `machines.yaml` class (and, for Q74, the recipe).

**Recommendation: gp-bwo's premise should be re-verified and most likely closed or rescoped to the
2 real data bugs (Q37's kind, Q74's missing recipe).** The 62-quest list is stale.

---

## 5. gp-s5r verification — **the premise is correctly CLOSED**

gp-s5r ("engine/sim has no own unit tests, openspec 2.4 deviation") is `✓ CLOSED` with the close
reason citing `cc96d599` and "engine_sim_components_test (186 tests), green in Debug and Release."

**Verified independently:**

```
$ ls src/engine/sim/test/
EngineSimComponents_test.cpp

$ ctest -N | grep engine_sim
  Test  #7: engine_sim_components_test
```

The test file exists at the path the close reason names, and it is a registered ctest target
(#7 of 54). **gp-s5r is genuinely fixed. No residual coverage deviation there.**

**One correction to the close reason, for the record:** the test lives at
`src/engine/sim/test/EngineSimComponents_test.cpp`, not `src/apps/simcore/test/` as the bead's
description suggested it might be moved *from* — so the move gp-s5r contemplated did happen. The
close reason is accurate.

---

## 6. Actionable recommendations, ordered

| # | Action | Closes |
|---|---|---|
| **0** | **Wire the two orphaned test files into CMake** (see row 5) — highest value per line of effort. | 3 requirements across 3 changes |
| 1 | **Add a recipe-id-format parser unit test** (3 formats + "numeric is rejected" + "no try/catch fallback detection"). No test exists at all for this surface. | `fix-ore-processing-chain` R1; `refactor-fluid-port-accounting` `recipe-id-format` R1+R2 (3 requirements) |
| 2 | **Add a `hbf_system_test` ctest target** next to `ebf_system_test`. The pattern is registered in 3 places and executed by none. | the prose of `separate-hbf-ebf-blast-furnaces-verification` task 4.1 |
| 3 | **Test the existing `tools/item_registry_cli.py validate` subcommand** — feed it overlapping sibling ranges, a duplicate packed id, and an out-of-range payload; assert non-zero exit. (`allocate`/`merge` do not exist yet, so task 4.4's dry-run half is blocked behind task 3.1.) | half of `refactor-item-registry-hierarchy` R5, today |
| 4 | **Add `specs/` deltas to the three spec-less changes.** They currently pass `openspec validate --strict` vacuously. | 3 changes, 16 unchecked tasks |
| 5 | **Wire the two orphaned test files into CMake** — `src/apps/game_client/tests/PipeFluidOverlay_test.cpp` (20,702 B, 14 test fns) and `tests/test_item_registry.cpp` (4,698 B). Copy the `WrenchOverlay_test.cpp` block at `CMakeLists.txt:290`. **Cheapest fix in this report: the tests already exist and already pass-by-construction; they simply never run.** | `add-pipe-fluid-overlay` R1; `refactor-item-registry-hierarchy`; `refactor-fluid-port-accounting` `architecture` R1 |
| 6 | **Reconcile stale `tasks.md` checkboxes** for `add-pipe-fluid-overlay` (0/9 but 5 tasks' code is in the tree) and `refactor-server-authoritative-inventory-verification` (tasks 5.2/5.3 already landed as `5e1e1c18`, `88d63621`). | openspec hygiene; makes future audits reliable |
| 7 | **Re-verify gp-bwo.** 62 → 2. Close or rescope to Q37 (wrong `kind`) and Q74 (missing `transformer_lv_mv` recipe). | gp-bwo |
| 8 | **Close gp-4re.7 as a duplicate of `add-multiplayer-foundation` §6.1-6.5.** | avoids duplicated work |
| 9 | **Merge or cross-link `complete-autonomous-mining` with `gp-4re.5`.** Same missing coverage. | avoids duplicated work |

**Not actionable (correctly orphaned, nothing implemented):** `add-chunk-interest-aggregator`
(3 reqs, 0/20), `add-multiplayer-foundation` (10 reqs, 0/24),
`refactor-fluid-port-accounting` ports/architecture (4 reqs, 0/30), `complete-autonomous-mining`
(3 reqs, 0/9). These changes schedule their own test tasks; that is the right shape.

---

## 7. Counts

- **14** active changes; **11** carry a spec delta; **3** do not.
- **42** requirements in those deltas; **35 (83%)** have no test exercising them.
- Of those 35, **9 are actionable today** (items 0-5 and 7 above), because they are either already
  implemented, written but never compiled, hard prohibitions with no enforcement test, or belong
  to a change with no spec to validate against.
- **26** are correctly orphaned pending implementation.
- `openspec validate <change> --strict`: **14/14 pass** (3 vacuously).
- gp-bwo: **62 → 2** real remaining data bugs. gp-s5r: **confirmed closed**, no deviation.
- **2 test files are written but never compiled**: `src/apps/game_client/tests/PipeFluidOverlay_test.cpp`
  (20,702 B, 14 test functions) and `tests/test_item_registry.cpp` (4,698 B). Neither appears in
  `src/apps/game_client/CMakeLists.txt`, so neither is in `ctest -N`'s 55 targets. This is a
  distinct failure mode from "no test" — the work is done and invisible.

---

## 8. The pattern behind the failures

Three of the nine actionable findings share one root cause, and it is **not** missing test
authoring:

1. **A test was written and never wired into the build** (§2.4). 14 test functions for the pipe
   fluid overlay, none of which has ever executed.
2. **`tasks.md` drifted behind the tree** (§2.4, §3). `add-pipe-fluid-overlay` reads 0/9 with a
   written-and-shipped test in the same directory; `refactor-server-authoritative-inventory-verification`
   still lists 5.2/5.3 as open although they landed as `5e1e1c18` and `88d63621` in today's log.
   An auditor who trusts `tasks.md` — which is the obvious thing to do — gets a false answer.
3. **A change with no `specs/` passes validation vacuously** (§3). `openspec validate --strict`
   returning OK means "the delta is well-formed", not "the requirement is verified"; for three
   changes there is no delta to be well-formed.

**The single highest-leverage action is therefore not writing a test.** It is adding a CI check
that every `*_test.cpp` under `src/` appears in some `add_executable`/`add_test` block. That one
check would have caught both unrun files, and it keeps catching them. The second is a
`tasks.md`-vs-`git log` reconciliation, which is mechanical but unglamorous and is what makes
every future audit of this kind trustworthy.
