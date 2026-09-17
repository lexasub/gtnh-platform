# Design: Engine layer separation (Stage 11)

Status: draft for user review. This document records the agreed boundary; no source moves yet.

## Decisions so far

- Split into multiple static libraries, not one monolithic engine archive.
- Final GTNH application links the modules it needs.
- Keep both ItemRegistry implementations; do not merge them in this change.
- Namespace mass-refactoring (rewriting item ids) is out of scope here.
- Runtime choice (.so vs VM, Q14) stays deferred; this change is the common seam.
- L0 content stays data-only.

## Proposed directory layout

Structure under `src/`:

```
src/
├── engine/                    # engine-core: knows nothing about GTNH
│   ├── registry/              # ItemId, strict Registry loader, validation
│   ├── sim/                   # ECS infrastructure, tick loop, pattern-matcher
│   │                          # machinery, condition mechanics
│   ├── net/                   # libgtnh-net moves here (io_uring, frames)
│   └── storage/               # storage_interfaces headers
├── game/                      # game rules: depends on engine, not vice versa
│   ├── machines/              # MachineSystem, Boiler, Generator, Transformer,
│   │                          # Turbine, Heat systems + MachineRegistry
│   ├── recipes/               # RecipeManager + ConditionEvaluator (second
│   │                          # registry consumer, no CSV parsing of its own)
│   ├── quests/                # quest_lib + QuestManager
│   └── mining/                # DrillSystem, block drops
├── content/                   # GTNH content pack (L0 + thin registration)
│   ├── data/                  # moved from data/: registry, recipes, quests,
│   │                          # patterns, fuels, fluids
│   └── content.cpp            # single place where concrete IDs and balance
│                              # are named in C++
└── apps/                      # assembly points
    ├── simcore/               # existing simulation_core daemon: links engine
    │                          # + game + content
    └── ...                    # other daemons as-is initially
```

## Move classes for the git mv script

Each file gets exactly one action:

- MOVE — whole file belongs to the new module; update includes and CMake target.
- EXTRACT — file mixes machinery and GTNH specifics; machinery stays, GTNH
  specifics move to `content/`; a new file is created there.
- KEEP — path already correct; no git mv.

Known candidates:

- MOVE: `src/libs/libgtnh-net` → `engine/net`; `src/libs/quest_lib` →
  `game/quests`; `src/libs/machine_registry` → `game/machines`;
  `src/libs/recipe_manager_lib` → `game/recipes`; `src/common` (ItemId,
  Registry, coords) → `engine/registry`.
- EXTRACT: `BoilerSystem.cpp` (machine-id compare `1110:011:1`),
  `GeneratorSystem.cpp` (fuel table coal/planks/stick), `FluidRegistry.cpp`
  (fluid defs in C++), `DrillSystem.cpp` (ore ids). Machinery stays in
  `game/`; id tables move to `content/content.cpp` or data files.
- KEEP: `GatewayMsg.h`, ResourcePort headers, protocol schemas; service mains
  until a later change.

## CMake target plan (created vs edited)

### Current targets (facts)

| Target | Today | Linked by |
|---|---|---|
| `gtnh_common` | STATIC, `src/common`, only Registry.cpp compiled | pipe_network, recipe_manager_lib |
| `gtnh-net` | STATIC, `src/libs/libgtnh-net` | gateway, chunkd, simcored, game_client |
| `machine_registry` | STATIC, yaml-cpp | simcored, game_client (+UI) |
| `quest_lib` | STATIC | simcored, game_client (+UI) |
| `recipe_manager_lib` | STATIC, own flatc step `recipe_lib_fbs`, links gtnh_common | simcored (+exec/test/Crafting), world_generator, reciped |
| `chunkd_storage` | inside chunk_store | chunkd, pipe_networkd compiles its sources directly (cross-service smell) |

Hidden include-path deps: simcored declares `target_include_directories(... src src/libs)` PUBLIC without linking gtnh_common; entity_state_store reaches storage_interfaces via include dir. DATA_DIR / REGISTRY_DATA_DIR compile definitions point at `${CMAKE_SOURCE_DIR}/data`.

### New targets (created)

| New target | Type | Sources | Replaces |
|---|---|---|---|
| `gtnh_engine_registry` | STATIC | engine/registry: Registry.cpp (+tests) | `gtnh_common` (renamed) |
| `gtnh_engine_storage` | INTERFACE | engine/storage headers (no .cpp) | include-path-only usage of storage_interfaces |
| `gtnh_engine_sim` | STATIC | engine/sim: extracted tick driver, pattern-matcher machinery, condition mechanics | new (later phase, EXTRACT) |
| `gtnh_game_machines` | STATIC | machines: MachineRegistry.cpp + moved system .cpp files | `machine_registry` (superseded) |
| `gtnh_game_recipes` | STATIC | recipes lib (keeps own flatc step `recipe_lib_fbs`) | `recipe_manager_lib` (renamed) |
| `gtnh_game_quests` | STATIC | quests lib + QuestManager.cpp | `quest_lib` (renamed) |
| `gtnh_game_mining` | STATIC | Drill, BatteryBuffer, CreativeGen, Adjacency .cpp | new |
| `gtnh_content` | STATIC | content.cpp only (thin registration; data is not compiled) | new |
| `gtnh_engine_sim` tests | per-target | engine/sim tests | new |

`gtnh-net` keeps its name; only its directory moves to `src/engine/net` (CMakeLists moves with it, no target rename to avoid churn).

### Edited CMake files

- `CMakeLists.txt` (root): `add_subdirectory(src/common)` and `add_subdirectory(src/libs)` → `add_subdirectory(src/engine)`, `add_subdirectory(src/game)`, `add_subdirectory(src/content)`; service dirs point to `src/apps/*`.
- NEW `src/engine/CMakeLists.txt` and per-module ones; same for `src/game/`, `src/content/`; `src/libs/CMakeLists.txt` and `src/common/CMakeLists.txt` deleted after moves.
- Service CMake files (now apps): replace library names with new targets; **remove `CMAKE_SOURCE_DIR}/src` and `/src/libs` blanket include dirs** — replaced by per-target PUBLIC includes from the new libs.
- pipe_network: stop compiling chunk_store Storage sources directly; link an explicit target (either exported `chunkd_storage` API target or a moved shared component — decision during phase 4).
- Compile definitions: `DATA_DIR="${CMAKE_SOURCE_DIR}/data"` → `src/content/data` in simcored_test, quest test; `REGISTRY_DATA_DIR` in registry test likewise; loader default paths updated in code in the same commit.
- Old targets `gtnh_common`, `machine_registry`, `quest_lib`, `recipe_manager_lib` are removed (no alias shims kept; all consumers updated in-tree in the same phase).

### Per-phase CMake ordering

1. Registry rename first (no dependents except pipe_network + recipe_manager_lib), ctest registry_test.
2. net path move (name unchanged) — reconfigure only, ctest gtnh-net tests.
3. storage INTERFACE target + entity_state_store include fix.
4. game libs: machines → recipes → quests → mining, one commit each, ctest after each.
5. content target last; only then flip service subdirectories to apps/.
- ctest passes after each module move, not batched.
