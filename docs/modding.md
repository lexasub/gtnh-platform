# Modding

> Skeleton — see `openspec/changes/refactor-engine-layer-separation` for the
> authoritative boundary decisions.

## Layers

```
src/engine/    # engine core: knows nothing about GTNH
               #   registry — ItemId, strict item Registry loader, OpenHashMap
               #   sim      — ISystem, SimulationEngine, PatternLibrary, ECS components
               #   net      — gtnh-net (io_uring connections, frame codec)
               #   storage  — header-only storage interfaces
src/game/      # game rules: depend on engine only, never vice versa
               #   machines, mining, quests, recipes
src/content/   # GTNH content pack: concrete ids + balance tables (L0 data + content.cpp)
src/apps/      # assembly points: daemons that link engine + game + content
```

Dependency direction: `apps → content → game → engine`. The engine does not
know mods.

## L0: data packs

Content lives in `src/content/data/` (registry, recipes, quests, textures).
An L0 pack is data-only — no C++:

```
src/content/data/
├── registry/    # items.csv, machines.yaml, ores.json — canonical catalog
├── recipes/     # one YAML per machine type
├── quests/      # quest_graph.json, quests.csv, requirements/rewards
└── textures/    # source packs + bindings.json
```

Compile-time ids use the same canonical catalog: `content/content.h` names
constants via `ItemId::pack("...")`, and balance tables (fuel values, ore
drops) live in `content/content.cpp` — the single C++ place naming balance.

## L1+: code mods — Q14 deferred

Loading compiled mods (`.so`) vs a sandboxed VM (Q14) is **deferred**. This
change builds only the common seam (layer separation + registration points).
Lua/Python runtimes and sidecar mods are permanently cut — C++-only modding.

## Anti-patterns

- ❌ Engine (`src/engine/`) referencing game/content headers
- ❌ GTNH ids or balance tables hardcoded in `game/` systems — register via `content.h`
- ❌ Apps including each other's internals — link the layer targets
