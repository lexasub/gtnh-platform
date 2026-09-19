## 1. Pre-flight

- [x] 1.1 Record baseline: `git status` clean, `ctest` green count, commit hash — rollback anchor.
- [x] 1.2 Generate the git mv script from design.md move classes: every file classified MOVE / EXTRACT / KEEP, script emits `git mv` + include-path rewrites per batch.

## 2. engine/ core modules (each batch: move → includes → CMake → ctest)

- [x] 2.1 `engine/registry`: move `src/common` (ItemId.h, Registry.{h,cpp}, coords/, OpenHashMap.h, test/, its CMakeLists.txt) to `src/engine/registry/`; rename CMake target `gtnh_common` → `gtnh_engine_registry`; update linkers pipe_network + recipe_manager_lib + registry_test; update REGISTRY_DATA_DIR path to src/content/data in same commit as data move (phase 4.1) or keep pointing at old data path until 4.1 lands.
- [x] 2.2 `engine/net`: git mv `src/libs/libgtnh-net` → `src/engine/net`; keep `gtnh-net` target name; no consumer edits (CMake picks up moved CMakeLists via add_subdirectory update in root).
- [x] 2.3 `engine/storage`: git mv storage_interfaces headers → `src/engine/storage/`; CREATE INTERFACE target `gtnh_engine_storage`; replace include-dir usage in entity_state_store with `target_link_libraries(... gtnh_engine_storage)`.
- [x] 2.4 `engine/sim`: EXTRACT from simulation_core (tick driver, pattern-matcher machinery, condition mechanics, chunk snapshot types) → `src/engine/sim/`, CREATE STATIC `gtnh_engine_sim` + tests; simcored links it, removes extracted .cpp from its own target sources.
- [x] 2.5 Core isolation check: engine targets build and their tests pass **without** game/ or content/ targets linked (ctest filter on engine tests only).
- [x] 2.6 Root CMakeLists: replace `add_subdirectory(src/common)` + `add_subdirectory(src/libs)` with `add_subdirectory(src/engine)`; delete src/common/CMakeLists.txt, src/libs/CMakeLists.txt. (Deviation: `src/common/` retained as `gtnh_wire` INTERFACE target — GatewayMsg.h / ResourcePort* / SlotContainer wire-protocol headers, consumed by gatewayd/gameclientd/machines/pipe_networkd/simcored; src/libs/ holds only Go module gtnh-common-go.)

## 3. game/ modules

- [x] 3.1 `game/recipes`: git mv `src/libs/recipe_manager_lib` → `src/game/recipes`; target `recipe_manager_lib` → `gtnh_game_recipes`; keep its own flatc step (`recipe_lib_fbs`) and links (gtnh_engine_registry, flatbuffers, yaml-cpp, SQLite, json); update consumers: simcored(+exec/test/Crafting), world_generator, reciped.
- [x] 3.2 `game/quests`: git mv `src/libs/quest_lib` → `src/game/quests` (lib under `game/quests/lib`), move QuestManager.{h,cpp} from simulation_core → `gtnh_game_quests` target; update game_client (+UI) and simcored links; quest test DATA_DIR updated.
- [x] 3.3 `game/machines`: CREATE STATIC `gtnh_game_machines`: MachineRegistry.cpp (from src/libs/machine_registry) + moved systems MachineSystem, BoilerSystem, GeneratorSystem, TransformerSystem, SteamTurbineSystem, RotareGeneratorSystem, LargeBoilerSystem, LCRSystem, EBFSystem, ExplosionSystem, CoolantSystem, BoilerPorts.h, HeatConstants.h; linkers simcored, game_client, game_client/UI switch from machine_registry.
- [x] 3.4 `game/mining`: CREATE STATIC `gtnh_game_mining`: DrillSystem, BatteryBufferSystem, CreativeGeneratorSystem, AdjacencyTransferSystem .cpp/.h from simulation_core/ECS/Systems; remove those sources from simcored target.
- [x] 3.5 EXTRACT pass: in BoilerSystem (machine-id `1110:011:1`), GeneratorSystem (fuel table), DrillSystem (ore ids), FluidRegistry (fluid defs): keep machinery, move id/balance tables to `src/content/` (data files or content.cpp); systems receive ids via registration.
- [x] 3.6 game layer check: game targets build linking only engine targets; no engine target links game. (Known exception, code-level only: engine/sim/SimulationEngine.cpp still wires game/machines systems and calls back into apps/simcore publishers — include paths granted, link stays clean. Tracked as follow-up debt.)

## 4. content/ + apps/

- [x] 4.1 `src/content/`: git mv `data/` → `src/content/data/` (registry, recipes, quests, textures, bindings); update DATA_DIR (simcored_test, quest test) and REGISTRY_DATA_DIR (registry_test) compile definitions + loader default paths in same commit.
- [x] 4.2 CREATE STATIC `gtnh_content`: content.cpp only (thin registration naming ids/fuels/fluids/machine configs); linked ONLY into apps; add `add_subdirectory(src/content)` in root.
- [x] 4.3 `src/apps/`: git mv each daemon dir (simulation_core → apps/simcore, chunk_store, gateway, entity_state_store, pipe_network, recipe_manager, game_client, world_generator, meta_db) updating root add_subdirectory paths; fix links to new layer targets; REMOVE blanket `${CMAKE_SOURCE_DIR}/src` and `/src/libs` include dirs from simcored (replaced by per-target PUBLIC includes); resolve pipe_network→chunk_store direct .cpp compilation (link explicit target instead); entitystated/gateway include paths updated.
- [x] 4.4 Full ctest green; run.sh smoke test (routerd → … → client boot order intact).

## 5. Docs + verification wave

- [x] 5.1 Update AGENTS.md structure section + README Project Structure; add docs/modding.md skeleton (layers, L0 pack example, Q14 note).
- [x] 5.2 Graphviz/C4 note: update doc/c4 level-2/3 mentions of library layout if present.
- [x] 5.3 Final verification: fresh `cmake --preset conan-debug`-equivalent configure in existing build dir, ninja build, full ctest, git diff shows no wire-format/protocol changes.
