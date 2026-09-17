# Change: Engine layer separation — static lib split (Stage 11)

## Why

The engine has no modding seam: GTNH-specific content (item ids, fuel tables, fluid definitions, machine ids) is hardcoded inside ECS systems and libraries, services link libraries through hidden include-path dependencies instead of CMake targets, and two item registry loaders with conflicting validation rules both parse the same items.csv. Agents and contributors cannot navigate engine vs game vs content because the boundaries exist only by convention. Stage 11's goal is: **the engine does not know mods** — three layers, C++-only modding (L0 data packs + L1 C++ in future), with Lua/Python runtimes and sidecar mods permanently cut (project.md's old "future scripting" note is superseded by user decision).

## What Changes

- Split into **multiple static libraries** (not one monolithic engine archive); the final GTNH application links the modules it needs.
- New source layout: `src/engine/` (registry, sim, net, storage), `src/game/` (machines, recipes, quests, mining), `src/content/` (GTNH L0 data + thin registration), `src/apps/` (assembly points that link engine + game + content).
- Dependency direction is enforced: game → engine only; content registers into game/engine via registrars; content.cpp is the single C++ place naming concrete ids/balance.
- A mechanical first pass via git mv script, classifying every file MOVE / EXTRACT / KEEP; EXTRACT files keep machinery in game/ and move id tables into content/.
- Keep BOTH existing ItemRegistry implementations (user decision): gtnh_common::Registry stays the strict canonical loader; recipe_manager_lib keeps its consumer-side ItemRegistry.
- Namespace mass-refactoring (rewriting item ids) is **out of scope**; uint16_t wire ids and items.csv canonical catalog are preserved (consistent with architecture spec and refactor-item-registry-hierarchy).
- Q14 runtime choice (.so vs VM) stays deferred; this change builds the common seam only.

## Impact

- Affected specs: `architecture` (new layer/dependency requirements added; service topology and data ownership unchanged), new capability `engine-layer-separation`.
- Affected code: `src/common`, `src/libs/*`, `src/services/simulation_core/ECS/Systems/*`, `src/services/pipe_network/FluidRegistry`, CMake target graph, include paths across services.
- Affected data: `data/` moves under `src/content/data/` (paths updated in loaders and CMake compile definitions).
- **BREAKING** for include paths (`#include "common/..."` → `#include "engine/registry/..."`) and CMake target names; no wire-protocol, save-format, or item-id changes.
- Build dirs `cmake-build-debug/`/`cmake-build-release/` keep their Conan toolchains; only reconfigure is needed.
