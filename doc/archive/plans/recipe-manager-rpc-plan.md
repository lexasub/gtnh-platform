# Plan: RecipeManager — Embedded → Standalone RPC Service

**Status**: Draft  
**Author**: Codebase analysis  
**Epic**: 0-foundation-recipe-system  
**Target branch**: main

---

## Table of Contents

1. [Current State](#1-current-state)
2. [Standalone Service Design](#2-standalone-service-design)
3. [Migration Path](#3-migration-path)
4. [File Manifest](#4-file-manifest)
5. [Risks and Open Questions](#5-risks-and-open-questions)

---

## 1. Current State

### 1.1 Where is RecipeManager right now?

RecipeManager is an **embedded library** inside SimulationCore:

```
src/services/simulation_core/RecipeManager/
├── RecipeManager.h / .cpp      # Core: Recipe struct, load/parse/check/craft/evaluate
├── RecipeConditions.h           # Condition data structures (Environment, Machine, Special)
├── ConditionEvaluator.h / .cpp  # 152-line condition evaluation engine
├── ItemRegistry.h / .cpp        # Singleton: item_id ↔ name mapping (CSV + SQLite)
```

**Loaded at startup** in `simulation_core/main.cpp:45`:

```cpp
auto recipeManager = std::make_shared<RecipeManager::RecipeManager>();
recipeManager->loadRecipesFromDirectory(recipes_dir);
```

**Three consumers** inside SimulationCore:

| Consumer | File | How it uses RecipeManager |
|----------|------|--------------------------|
| `CraftRequestHandler` | `Crafting/CraftRequestHandler.cpp` | Owns a `RecipeManager::RecipeManager` member. Subscribes to `sim.craft.request` topic. Calls `recipeManager_.getRecipes()`, `getRecipeById()`, `recipe.craft()`. Publishes response on `sim.craft.response`. |
| `MachineSystem` (ECS tick) | `ECS/Systems/MachineSystem.cpp` | Receives `shared_ptr<RecipeManager>`. Calls `findRecipeByInputs()`, `getRecipeById()`, `recipe.craft()` during 20 Hz tick. Works with real ECS components (`RecipeProgress`, `InventoryContainer`, `EnergyStorage`). |
| `WorkbenchStateManager` / `GridPatternMatcher` | `Crafting/` | Uses RecipeManager's internal `Recipe` struct and `ItemStack` for grid matching and preview. |

**Recipe data sources** — 6 JSON files in `data/recipes/`:
- `crafting_table.json` (MachineType::NONE)
- `furnace.json`
- `assembler.json`
- `crystallizer.json`
- `electrolyser.json`
- `chemical_reactor.json`

### 1.2 What protocol already exists

`src/protocol/recipe.fbs` defines the full RPC protocol:

```
RecipeFrame → RecipeMessage { req_id, RecipeRequest(CheckRecipeReq|CraftReq|EvaluateConditionsReq) }
RecipeFrame → RecipeReply    { req_id, RecipeResponse(CheckRecipeResp|CraftResp|EvaluateConditionsResp|ErrorResp) }
```

The FlatBuffers schema is **generated** by both `simcored_fbs` and any new service's FBS target. `recipe_generated.h` is already included by `RecipeManager.h`.

**kCraftRequest=9** and **kCraftResponse=10** exist in the core protocol (`src/protocol/simcore.fbs` or `core.fbs`) — these are the **old** gateway→simulation_core path for workbench crafting, which flows through `GatewayPayload` → `CraftRequest` → `CraftRequestHandler`.

### 1.3 How CraftRequest currently flows

```
GameClient ──TCP──→ Gateway ──publish "sim.craft.request"──→ SimulationCore
                                                              ↓
                                                      CraftRequestHandler
                                                              ↓
                                                      RecipeManager (embedded)
                                                              ↓
                                                  publish "sim.craft.response"
```

The recipe.fbs RPC protocol (`CheckRecipeReq` etc.) is **not used on the wire today**. The `RecipeManager::handle*Request()` methods exist but are never called from message dispatch — they are an implemented RPC shim waiting for the standalone service.

### 1.4 Current dependencies

| Library | Used by RecipeManager | Why |
|---------|----------------------|-----|
| `flatbuffers` | Included + generated headers | Serialization for protocol |
| `spdlog` | Logging during load/match/craft | |
| `nlohmann/json` | JSON recipe file parsing | |
| `filesystem` (std) | Directory iteration for recipe loading | |
| `sqlite3` | ItemRegistry SQLite loading | |
| `ConditionEvaluator` | Internal evaluation (no MachineState data) | |

---

## 2. Standalone Service Design

### 2.1 Architecture

```
┌──────────────┐     recipe.check / recipe.craft / recipe.evaluate     ┌──────────────────┐
│ Simulation   │───────────────────────────────────────────────────── ►│  RecipeManager   │
│ Core         │◄──────────────────────────────────────────────────── ─│  (standalone C++)│
│              │     recipe.check.response / recipe.craft.response     │                  │
│              │                                                       │  Loads from      │
│  (embedded   │                                                       │  data/recipes/*.json
│   fallback)  │     recipe.hotreload (admin command)                  │                  │
│              │◄──────────────────────────────────────────────────────│  ItemRegistry    │
└──────────────┘                                                       │  (csv → SQLite)  │
                                                                       │                  │
┌──────────────┐     recipe.check (recipe book preview)                │  ConditionEvaluator
│ GameClient   │──────────────────────────────────────────────────── ─►│                  │
│ (via Gateway)│◄──────────────────────────────────────────────────── ─┘  TCP: 5300       │
└──────────────┘                                                       └──────────────────┘
```

RecipeManager becomes a **first-class MessageRouter peer** (like EntityStateStore). It:
1. Registers with MessageRouter as `"reciped"`
2. Subscribes to `recipe.check`, `recipe.craft`, `recipe.evaluate`
3. Publishes responses on `recipe.check.response`, `recipe.craft.response`, `recipe.evaluate.response`
4. Optionally listens on dedicated TCP port 5300 for direct RPC (same pattern as EntityStateStore port 5200)

### 2.2 Protocol routing

**Topics for RPC** (following the entity_state_store naming convention):

| Topic | Payload (FlatBuffers root) | Response topic |
|-------|---------------------------|----------------|
| `recipe.check` | `RecipeFrame` → `RecipeMessage` → `CheckRecipeReq` | `recipe.check.response` |
| `recipe.craft` | `RecipeFrame` → `RecipeMessage` → `CraftReq` | `recipe.craft.response` |
| `recipe.evaluate` | `RecipeFrame` → `RecipeMessage` → `EvaluateConditionsReq` | `recipe.evaluate.response` |
| `recipe.hotreload` | (command to reload JSON from disk) | none / ack |

The response uses the same `RecipeFrame` wrapper with the matching `req_id`.

**Why topic-per-method instead of a single `recipe.rpc` topic?**
- Allows fine-grained subscribe (services only listen to what they need)
- Follows the existing entity_state_store pattern (`entity.state.get`, `entity.state.set`)
- Avoids a sub-dispatch switch inside the subscriber callback

### 2.3 Service structure

```
src/services/recipe_manager/
├── CMakeLists.txt
├── main.cpp                    # Entry point, wire up Router + TCP
├── RecipeManagerService.h/cpp  # Wraps RecipeManager with RPC dispatch
├── RecipeManager/               # Copied from simulation_core or linked as static lib
│   ├── RecipeManager.h/.cpp
│   ├── RecipeConditions.h
│   ├── ConditionEvaluator.h/.cpp
│   └── ItemRegistry.h/.cpp
└── Client/
    └── MessageRouterClient.h/.cpp  # (copied from entity_state_store or shared)
```

**Two options for sharing RecipeManager code**:

**Option A (recommended for MVP) — Copy**: Copy `RecipeManager/` directory verbatim into the new service's source tree. No shared library build system complexity. Both copies load JSON independently. During migration, the embedded copy in SimCore stays as-is. After proving the standalone service works, the embedded copy becomes a thin RPC client.

**Option B — Static library**: Extract `RecipeManager/` into a shared CMake library target that both `simcored` and `reciped` link. Requires build system refactoring and coordination. Better in the long term but blocks the MVP.

**Recommendation**: Start with Option A. The standalone service needs zero changes to the existing RecipeManager code initially — it's clean C++ with no SimulationCore-specific dependencies (no EnTT, no io_uring). The only thing to add is the RPC server loop.

### 2.4 Service lifecycle (startup)

```
1. Load ItemRegistry from CSV/SQLite
2. Load all recipes from data/recipes/*.json
3. Register with MessageRouter as "reciped"
4. Subscribe to recipe.check, recipe.craft, recipe.evaluate
5. (optional) Start TCP acceptor on port 5300
6. Main loop: dispatch RPC requests → RecipeManager → send replies
```

### 2.5 Startup order

```
routerd → chunkd → reciped → gatewayd → simcored → client
```

`reciped` must start **before** `simcored`, because on startup SimCore currently loads recipes itself (embedded). After migration to use remote RPC, SimCore will need the standalone RecipeManager to be available at startup. But during the **dual-run phase** (embedded + standalone), order doesn't matter — SimCore uses its embedded copy.

### 2.6 L1 cache in SimulationCore (ADR consideration)

The existing ADR mentions an L1 recipe cache in SimCore to avoid RPC roundtrips on every tick. During the dual-run phase, measure the RPC latency to determine if caching is necessary:

- `CheckRecipe`: Called every tick (20 Hz) per idle machine. A cache keyed by `(machine_type, container_hash)` with TTL of 1-5 seconds can absorb bursts.
- `Craft`: Called once per recipe completion. Low frequency, no caching needed.
- `EvaluateConditions`: Called once per recipe start. Low frequency.

If the standalone service runs on localhost (RTT < 0.1 ms), caching may be unnecessary for the MVP. Revisit after profiling.

### 2.7 Hot-reload of recipes

A `recipe.hotreload` topic (or IPC signal) triggers the standalone service to re-scan `data/recipes/` and reload all JSON files. The service holds a `std::shared_ptr<const RecipeManager>` and atomically swaps to a new instance after loading succeeds. This prevents:

- Half-loaded state on parse error
- Clients reading stale data during reload

```cpp
std::shared_ptr<const RecipeManager> current_;
std::mutex mtx_;

void reload() {
    auto fresh = std::make_unique<RecipeManager>();
    if (fresh->loadRecipesFromDirectory("data/recipes/")) {
        std::lock_guard lock(mtx_);
        current_ = std::move(fresh);
        spdlog::info("RecipeManager: hot-reloaded {} recipes", current_->recipeCount());
    } else {
        spdlog::error("RecipeManager: hot-reload FAILED, keeping old state");
    }
}
```

### 2.8 ConditionEvaluator — shared library vs RPC

**Recommendation**: Keep `ConditionEvaluator` as a **copied source file**, not a separate RPC boundary.

Rationale:
- Condition evaluation is cheap (~10 condition checks, no I/O, no allocations beyond `MachineState`)
- It requires machine state (temperature, energy, facing) that only SimulationCore has
- Calling EvaluateConditions over RPC would require serializing MachineState into FlatBuffers, sending, evaluating, and returning — adding latency to the hot machine tick path
- The existing `EvaluateConditionsReq` protocol is still valid for the standalone service's "recipe book preview" use case where no machine state is available

The standalone RecipeManager can serve `EvaluateConditionsReq` for recipe book/display purposes (returning whether conditions *could* be satisfied in theory). The actual per-tick evaluation stays inline in SimCore's MachineSystem.

---

## 3. Migration Path

### Phase 0: Preparation (no behavior change)

- Create `src/services/recipe_manager/` directory structure
- Copy RecipeManager source files (Option A)
- Create CMakeLists.txt, main.cpp with MessageRouter registration
- Verify standalone service compiles, loads recipes, and idles

**No changes to SimulationCore**. Both copies of RecipeManager load JSON independently.

### Phase 1: Dual-run — standalone serves alongside embedded

- SimulationCore's `MachineSystem` learns to call remote RecipeManager **if available**, falling back to embedded
- Add a `RecipeManagerClient` class in SimCore that wraps the RPC call across topics
- `CraftRequestHandler` switches to publishing `recipe.craft` instead of calling embedded `recipeManager_.craft()`
- Wire the RPC replies back into SimCore's machine tick

**CraftRequestHandler migration** (old → new):

```
// OLD: direct embedded call
recipeManager_.getRecipes()
recipeManager_.getRecipeById(matchedId)
recipe.craft(items)

// NEW: publish request on topic, async callback
router->Publish("recipe.craft", frame);
// reply handled in OnMessage → topic "recipe.craft.response"
```

**MachineSystem migration** (old → new):

```
// OLD: shared_ptr<RecipeManager> recipes_
recipes_->findRecipeByInputs(machine_type, inputItems)
recipes_->getRecipeById(progress.recipe_id)
recipe->craft(inputItems)

// NEW: async RPC call
engine.enqueueCraftCheck(machine_type, container_snapshot);
// reply sets progress.recipe_id, remaining_ticks
```

Async RPC in the tick loop is acceptable because CheckRecipe is only called when a machine is idle (no running recipe). The response arrives via the OnMessage callback and sets the recipe progress.

### Phase 2: Embedded → thin client

- Remove the `RecipeManager/RecipeManager.cpp` (the heavy JSON-loading code) from SimulationCore's CMakeLists.txt
- Replace with a lightweight `RecipeManagerClient` that:
  - Makes RPC calls over MessageRouter topics
  - (Optional) Implements the L1 cache
  - Has a local `ConditionEvaluator` for the hot path

This phase is **optional** — the embedded copy can stay as long as it remains useful. Remove only after:
- Standalone service has been stress-tested at 20 Hz with multiple machines
- No regression in craft throughput
- Client-side recipe book works via RPC

### Phase 3: Direct TCP RPC (optional)

- The standalone service opens TCP port 5300 (like EntityStateStore port 5200)
- Clients can bypass MessageRouter for lower latency
- Useful for GameClient recipe book queries during world load

---

## 4. File Manifest

### New files to create

| # | File | Purpose |
|---|------|---------|
| 1 | `src/services/recipe_manager/CMakeLists.txt` | Build definition for `reciped` executable |
| 2 | `src/services/recipe_manager/main.cpp` | Entry point: init, register with router, main loop |
| 3 | `src/services/recipe_manager/RecipeManagerService.h` | RPC dispatch: subscribes to topics, calls RecipeManager, publishes replies |
| 4 | `src/services/recipe_manager/RecipeManagerService.cpp` | |
| 5 | `src/services/recipe_manager/RecipeManager/RecipeManager.h` | Copied from simulation_core |
| 6 | `src/services/recipe_manager/RecipeManager/RecipeManager.cpp` | |
| 7 | `src/services/recipe_manager/RecipeManager/RecipeConditions.h` | |
| 8 | `src/services/recipe_manager/RecipeManager/ConditionEvaluator.h` | |
| 9 | `src/services/recipe_manager/RecipeManager/ConditionEvaluator.cpp` | |
| 10 | `src/services/recipe_manager/RecipeManager/ItemRegistry.h` | |
| 11 | `src/services/recipe_manager/RecipeManager/ItemRegistry.cpp` | |
| 12 | `src/services/recipe_manager/Client/MessageRouterClient.h` | Copied from entity_state_store (or shared) |
| 13 | `src/services/recipe_manager/Client/MessageRouterClient.cpp` | |

### Files to modify

| # | File | Change |
|---|------|--------|
| 14 | `src/services/simulation_core/ECS/Systems/MachineSystem.h` | Optionally add `RecipeManagerClient` for remote RPC |
| 15 | `src/services/simulation_core/ECS/Systems/MachineSystem.cpp` | Phase 1: call remote CheckRecipe at idle tick |
| 16 | `src/services/simulation_core/Crafting/CraftRequestHandler.h` | Replace embedded RecipeManager with RPC client |
| 17 | `src/services/simulation_core/Crafting/CraftRequestHandler.cpp` | |
| 18 | `src/services/simulation_core/CMakeLists.txt` | Phase 2: remove RecipeManager .cpp files from source list |
| 19 | Top-level `CMakeLists.txt` | Add `add_subdirectory(src/services/recipe_manager)` |
| 20 | `build/` (README / startup scripts) | Add `./build/reciped` to startup order |

### No protocol changes needed

The existing `recipe.fbs` already defines the full RPC interface. No new FlatBuffers tables are required.

---

## 5. Risks and Open Questions

### 5.1 Risks

| # | Risk | Likelihood | Impact | Mitigation |
|---|------|-----------|--------|------------|
| R1 | RPC latency causes machine tick starvation at 20 Hz | Medium | High | L1 cache in SimCore; inline ConditionEvaluator for hot path; localhost RTT < 0.1ms |
| R2 | Two copies of data (recipes loaded by both services) diverge | Medium | Medium | Single source of truth = JSON files. Use recipe file modification timestamps + hot-reload to detect drift |
| R3 | ConditionEvaluator needs machine state that the standalone service doesn't have | Low | Medium | Keep ConditionEvaluator inlined in SimCore for hot path; standalone uses empty MachineState for recipe book preview only |
| R4 | MessageRouter becomes bottleneck for 20 Hz CheckRecipe calls | Low | Low | Each call is a tiny FlatBuffer (~64 bytes); router handles 100k topics at scale |
| R5 | Hot-reload atomic swap race with concurrent RPC | Low | Medium | Use `shared_ptr` atomic swap with `load()` / `store()`. RPC handlers operate on a snapshot of the pointer |

### 5.2 Open Questions

| # | Question | Decision needed |
|---|----------|----------------|
| Q1 | Should the standalone service use **asio** (like EntityStateStore) or **io_uring** (like SimCore) for the Router client? | asio is simpler for a mostly-idle RPC server. io_uring only justified if benchmarking shows >10k RPS. **Recommend: asio**. |
| Q2 | Should RecipeManager use its own **flatc** generated headers or reuse simcored_fbs output? | If the build is top-level CMake, each service generates its own. If we extract `recipe_generated.h` to a shared location, all services can share. **Recommend: own generation** (follows entity_state_store pattern). |
| Q3 | When should the embedded RecipeManager be removed? | After the standalone service has been running in production for 1 week with zero recipe mismatches. |
| Q4 | Should the GameClient talk to RecipeManager **directly** (TCP port 5300) or **via Gateway** (MessageRouter)? | Via Gateway during MVP (no new client connections). Direct TCP as Phase 3 optimization. |
| Q5 | Should the `recipe.hotreload` topic be an admin RPC or an internal timer? | Admin RPC triggered by operator command. Future: `inotify` on the directory. |
| Q6 | What happens if the standalone RecipeManager service crashes? | SimCore's embedded copy (in fallback mode) continues serving. After migration (Phase 2), SimCore would lose recipe lookups → machines stall. Add reconnect + retry logic in the RPC client. |
| Q7 | Should the standalone service persist recipe usage statistics? | Not for MVP. Future: log recipe match frequency for balancing. |
| Q8 | Where should the `ConditionEvaluator` live long-term? | **Recommend**: Keep a copy in SimCore for hot path (inline), and a copy in the standalone service for recipe book / preview use. They are the same code — no divergence risk since the evaluation logic is simple and stable. |

### 5.3 Key constraints reaffirmed

- ❌ No new protocol fields unless required
- ❌ No new external dependencies
- ✅ `recipe.fbs` RPC protocol is sufficient as-is
- ✅ Embedded RecipeManager stays as fallback until standalone is proven
- ✅ ConditionEvaluator can be shared as copied source (not RPC)
- ✅ Startup order: routerd → reciped → simcored

---

## Appendix A: Comparison with EntityStateStore

EntityStateStore is the best reference for the standalone RecipeManager service:

| Aspect | EntityStateStore | RecipeManager (planned) |
|--------|-----------------|------------------------|
| Protocol schema | `entity_state_store.fbs` | `recipe.fbs` (already done) |
| Router client | `MessageRouterClient` (asio) | Same pattern, copied |
| Message dispatch | `OnMessage` → switch topics | Same pattern |
| TCP RPC port | 5200 | 5300 |
| Persistence | LMDB | None (read-only from JSON) |
| Service name | `entitystated` | `reciped` |

The RouterClient code from `entity_state_store/Client/` can be copied verbatim — it already has all the necessary frame helpers, heartbeat, and reconnection logic.

## Appendix B: RPC wire format reference

For `recipe.check`:

```
GameClient/SimCore ──→ Publish("recipe.check", frame) ──→ MessageRouter ──→ RecipeManager
                                                                              ↓
                                                                    parse RecipeFrame
                                                                         ↓
                                                                    handleCheckRecipeReq()
                                                                         ↓
                                                                    Publish("recipe.check.response", reply)
```

Where:
- `frame` = FlatBuffer with `RecipeFrame` → `RecipeMessage` → `CheckRecipeReq`
- `reply` = FlatBuffer with `RecipeFrame` → `RecipeReply` → `CheckRecipeResp`
