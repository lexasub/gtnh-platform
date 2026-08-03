# Change: Fix Ore Processing Chain Bugs

## Why

Userflow 07 describes ore→macerator→furnace→compressor chain. Current recipes are broken at every level:

### Bug 1: Hardcoded Numeric IDs (root cause)
Recipe YAML files use **hardcoded flat decimal IDs** (e.g., `item: 3` for "iron ore", `item: 4` for "iron ingot"). This is the root cause:
- `item: 3` → `std::stoi("3")` → uint16_t(3) → `ItemId::pack("0:0:3")` = **sand**, not iron ore
- `item: 4` → uint16_t(4) → **glass**, not iron ingot
- `item: 9` → **not registered** in items.csv

Correct values via `ItemId::pack()`:
- iron_ore (`10:0`) = **32768**
- iron_ingot (`0:110:1`) = **24577**

**Solution**: No hardcoded numeric IDs anywhere. Recipe YAML MUST use string item names (`item: iron_ore`) resolved at runtime via `ItemRegistry::nameToId()` → `ItemId::pack()`. The YAML parser already supports this path (`RecipeManager.cpp:468`: `item.item_id = resolveItemName(itemStr)` on stoi failure).

### Bug 2: Compressor same-I/O (`data/recipes/compressor.yaml`)
`compress_iron` outputs `item: 4` (iron_ingot) — same as input. Should output iron_plate. No iron_plate exists in items.csv.

### Bug 3: Macerator same-output (`data/recipes/macerator.yaml`)
All 7 non-iron ore types output `item: 9` (undefined dust) regardless of input. Each ore type needs its own crushed variant.

### Bug 4: Furnace missing recipes (`data/recipes/furnace.yaml`)
Only has iron_ore→iron_ingot and gold_ore→iron_ingot. Missing:
- All `crushed_X→X_ingot` recipes (needed after macerator)
- All `copper/tin/lead/silver/zinc ore→ingot` recipes

### Bug 5: Items missing from registry (`data/registry/items.csv`)
No crushed ore items, no metal plates (except bronze_plate). Must be added before recipes can reference them.

## What Changes

### 1. No hardcoded numeric IDs — always use string names
All `item:` fields in recipe YAML MUST use string names (e.g., `item: iron_ore`, `item: crushed_iron`, `item: iron_ingot`, `item: iron_plate`). These resolve at runtime:
```
parseYamlInputItem → stoi("iron_ore") fails → resolveItemName("iron_ore") → ItemRegistry::nameToId("iron_ore") → packed uint16_t via ItemId::pack()
```
The parser already supports this (`RecipeManager.cpp:464-468`). No flat decimal IDs anywhere.

This means:
- `data/registry/items.csv` is the single source of truth — all item definitions live there with correct hierarchical IDs
- Recipe files reference items by string name only
- The `ItemId::pack()` system validates correctness at startup

### 2. Add missing items to `data/registry/items.csv`
- Crushed ores: crushed_iron, crushed_gold, crushed_copper, crushed_tin, crushed_lead, crushed_silver, crushed_zinc
- Metal plates: iron_plate, gold_plate, copper_plate, tin_plate, lead_plate, silver_plate, zinc_plate
- Use appropriate MATERIALS prefix (0:1110: sub-prefix) with correct `ItemId::pack()` values

### 3. Fix `data/recipes/macerator.yaml`
- Replace ALL hardcoded numeric `item:` values with string names (e.g., `item: iron_ore`, `item: crushed_iron`)
- Each ore type outputs its own crushed variant (not generic dust)
- Add lead, silver, zinc ore recipes
- Ensure HEAT/STEAM/ROTATION energy variants exist for core ores

### 4. Fix `data/recipes/furnace.yaml`
- Replace ALL hardcoded numeric `item:` with string names
- Add crushed_X→X_ingot recipes for all metal types
- Add missing ore→ingot recipes for copper, tin, lead, silver, zinc

### 5. Fix `data/recipes/compressor.yaml`
- Replace ALL hardcoded numeric `item:` with string names
- Fix compress_iron output from numeric 4 to string `iron_plate`
- Add X_ingot→X_plate recipes for all metal types

## Impact

- **Affected specs**: `ore-processing` (new — created by this change)
- **Affected data files**:
  - `data/registry/items.csv` — add ~14 new items (crushed ores + plates), ensure all entries have correct `ItemId::pack()` hierarchical IDs for string-based resolution
  - `data/recipes/macerator.yaml` — replace all numeric IDs with string names, add missing ore recipes
  - `data/recipes/furnace.yaml` — replace all numeric IDs with string names, add 14+ new recipes
  - `data/recipes/compressor.yaml` — replace all numeric IDs with string names, fix 1 bug, add 6+ recipes
- **No impact**: Protocol, ECS, pipe network, simulation tick, UI (item IDs flow transparently via ItemRegistry)
- **Guarantee**: Zero hardcoded numeric item IDs in recipe YAML after this change

## Reference Map

### Item ID Resolution Chain
Recipe YAML uses string names → resolved at runtime → no hardcoded IDs:
```
recipe.yaml: item: iron_ore
  → RecipeManager::parseYamlInputItem() or parseYamlOutputItem()
  → std::stoi("iron_ore") throws
  → resolveItemName("iron_ore")
  → ItemRegistry::nameToId("iron_ore")
  → ItemId::pack() NOT called here, but the ID in items.csv was stored via pack()
```

`data/registry/items.csv` is the single source of truth. Each line is loaded via:
```
ItemRegistry::loadFromCSV(line)
  → ItemId::pack(idStr)  // e.g., "10:0" → 32768
  → itemsById_[32768] = {name: "iron_ore", ...}
  → itemsByName_["iron_ore"] = 32768
```

### How Recipe YAML Must Reference Items

**CORRECT** (string name — resolves via ItemRegistry):
```yaml
inputs:
  - { item: iron_ore, count: 1 }
outputs:
  - { item: crushed_iron, count: 2 }
```

**WRONG** (hardcoded numeric — produces wrong item or breaks):
```yaml
inputs:
  - { item: 3, count: 1 }   # sand, not iron_ore!
```

### Key Items to Add to `items.csv`
| Name | Hierarchical ID | `ItemId::pack()` |
|------|----------------|-------------------|
| crushed_iron | TBD (MATERIALS prefix) | computed at implementation |
| crushed_gold | TBD | computed at implementation |
| crushed_copper | TBD | computed at implementation |
| iron_plate | TBD | computed at implementation |
| copper_plate | TBD | computed at implementation |

Implementation agent MUST run `ItemId::pack()` via test program (as done in `src/common/ItemId.h:84`) to compute correct uint16_t values for new entries, ensuring no collisions with existing IDs.

### Code References
| File:Line | Content |
|-----------|---------|
| `src/common/ItemId.h:84` | `ItemId::pack()` — hierarchical→uint16_t conversion |
| `src/libs/recipe_manager_lib/ItemRegistry.cpp:56` | CSV loads via `ItemId::pack()` per line |
| `src/libs/recipe_manager_lib/ItemRegistry.cpp:189` | `nameToId()` — string→uint16_t lookup |
| `src/libs/recipe_manager_lib/RecipeManager.cpp:464-468` | `parseYamlInputItem()`: tries `stoi()` first, falls back to `resolveItemName()` on failure |
| `src/libs/recipe_manager_lib/RecipeManager.cpp:506-510` | Same `stoi()`→`resolveItemName()` fallback for outputs |
| `src/services/recipe_manager/main.cpp:60` | Items CSV loaded at startup |
| `src/services/simulation_core/ECS/Systems/MachineSystem.cpp` | Tick loop — Pass 1 recipe match, Pass 2 output+pipe push |
| `src/libs/machine_registry/MachineRegistry.cpp:95` | Block ID parsed as uint16_t from YAML (note: "1110:00:1" → 1110, not packed) |

### Spec & Related Change References
- `openspec/specs/architecture/spec.md` — service topology
- `openspec/specs/protocol/spec.md` — wire format, message types
- `openspec/changes/add-player-interaction/specs/player-interaction/spec.md` — Crafting Pipeline
- `openspec/changes/implement-pipes-cables-transport/specs/implement-pipes-cables-transport/spec.md` — Item pipe transport
- `openspec/changes/archive/2026-07-12-implement-ore-generation/specs/implement-ore-generation/spec.md` — Ore generation (upstream input)

### Implementation Flow
1. `RecipeManager::loadRecipesFromYamlDirectory("data/recipes/")` → parses → `recipes_` map
2. `MachineSystem::tick()` Pass 1: `findRecipeByInputs(block_id, inputItems)` by class+tier+energy_type
3. Pass 2: on recipe completion → `pushOutputToPipe()` → `ItemClient::publishNodeUpdate()` → PipeNetwork
4. PipeNetwork BFS transports items to machine
5. `ItemFlowHandler` delivers to machine input slots
