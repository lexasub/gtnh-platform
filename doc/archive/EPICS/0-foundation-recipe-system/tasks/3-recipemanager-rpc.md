# TASK: RecipeManager RPC Interface
**Layer**: 0
**Status**: Draft
**Epic**: 0-foundation-recipe-system

## Affected Services

| Service | Role | Direction |
|---------|------|-----------|
| **RecipeManager** ⬅️ NEW | Server — exposes CheckRecipe + Craft RPCs | Responds |
| **SimulationCore** | Client — calls CheckRecipe on idle machine, Craft on completion | Calls |
| **GameClient** | Client — can query RecipeManager directly (e.g., recipe book GUI) | Calls |
| **MessageRouter** | Transport — routes RPC frames | — |

> **Architecture rule**: RecipeManager is a first-class MessageRouter peer. SimulationCore is one consumer, but *any* service (including Gateway/GameClient) can call RecipeManager directly.

---

## Overview

RecipeManager exposes two RPC methods via MessageRouter: `CheckRecipe` and `Craft`. Both operate on a `Container` (holds `ItemStack` entries) and use a `MachineType` constant. The RPCs are published as FlatBuffers messages over TCP.

- **CheckRecipe** — SimulationCore queries RecipeManager before starting a machine cycle. Returns the recipe ID if the container matches a recipe, otherwise `null`.
- **Craft** — A machine (furnace, assembler, etc.) calls this when conditions are met. Consumes inputs, produces outputs, and returns the updated container.

---

## FlatBuffer Schema

### Protocol namespace

All tables live under `namespace Protocol`.

### MachineType

```flatbuffers
enum MachineType {
  NONE        = 0,
  FURNACE     = 1,
  ASSEMBLER   = 2,
  CRYSTALLIZER= 3,
  ELECTROLYSER= 4,
  CHEMICAL_REACTOR= 5,
  MAX         = 6,
};
```

### ItemStack

```flatbuffers
struct ItemStack {
  uint16_t item_id;  // item registry ID
  uint8_t  count;    // quantity
  uint8_t  meta;     // damage/data or auxiliary flags
};
```

### Container

A flat array of `ItemStack` entries. Fixed length 9 for inventory-based machines, 256 for chest-based machines.

```flatbuffers
struct Container {
  ItemStack items[9];  // slot 0 = machine output/input, slots 1-8 = inventory
  uint8_t  dirty;      // flag set when any item changes
  uint16_t size;       // number of valid entries
};
```

### CheckRecipeRequest

```flatbuffers
table CheckRecipeRequest {
  Container  container   : offset;  // current machine contents
  uint8_t    machine_type: MachineType;
};
```

### CheckRecipeResponse

```flatbuffers
table CheckRecipeResponse {
  string? recipe_id : null;  // recipe ID if matched, otherwise null
};
```

### CraftRequest

```flatbuffers
table CraftRequest {
  string  recipe_id   : offset;  // validated recipe ID
  Container container : offset;  // machine contents
};
```

### CraftResponse

```flatbuffers
table CraftResponse {
  Container new_container : offset;  // updated contents after crafting
};
```

---

## Message Flow

### CheckRecipe

1. **SimulationCore** calls `CheckRecipe(container, machine_type)` when a machine finishes its tick or when an item is placed in its input slot.
2. RecipeManager compares the container against its recipe database for the given `MachineType`.
3. RecipeManager publishes `CheckRecipeResponse` to MessageRouter.
4. SimulationCore reads the response and decides whether to start the machine cycle.

```
SimulationCore ──→ RecipeManager ──→ MessageRouter ──→ SimulationCore
    |                    |                  |
    |                    |                  └─→ CheckRecipeResponse.recipe_id
    |                    |
    └───────── container ─┘
         machine_type
```

### Craft

1. **Machine service** (furnace, assembler, etc.) calls `Craft(recipe_id, container)` when all preconditions are satisfied.
2. RecipeManager validates `recipe_id` against its database, then applies the recipe to the container.
3. RecipeManager publishes `CraftResponse` to MessageRouter.
4. The machine service receives the updated container and writes it back to the world via ChunkStore.

```
Machine ──→ RecipeManager ──→ MessageRouter ──→ Machine
    |                    |                  |
    |                    |                  └─→ CraftResponse.new_container
    |                    |
    └──────── recipe_id ─┘
               container
```

---

## RPC Table

| Method    | Inputs                                          | Output              |
|-----------|-------------------------------------------------|---------------------|
| CheckRecipe | `container: Container`, `machine_type: MachineType` | `recipe_id: string` |
| Craft      | `recipe_id: string`, `container: Container`      | `new_container: Container` |

---

## File Locations

| File                              | Description                              |
|-----------------------------------|------------------------------------------|
| `src/protocol/recipe.fbs`         | FlatBuffers schema (tables above)        |
| `src/services/recipe_manager/`    | RecipeManager service implementation     |
| `doc/EPICS/0-foundation-recipe-system/0-foundation-recipe-system.md` | Epic spec |

---

## Acceptance Criteria

#### Scenario 1: Furnace smelts iron ore

- **Setup**: Furnace container with 1 iron ore in slot 0, all other slots empty.
- **Action**: SimulationCore calls `CheckRecipe(container, FURNACE)`.
- **Expected**: Response returns `"gt:smelting:iron_ore"`.
- **Follow-up**: Furnace calls `Craft("gt:smelting:iron_ore", container)` after temperature reaches 1300°C.
- **Expected**: Response returns container with iron ingot in slot 0, coal in slot 1.

#### Scenario 2: Assembler crafts a machine part

- **Setup**: Assembler container with 2 copper plates and 2 iron plates in slots 0-3, remaining slots empty.
- **Action**: Machine service calls `CheckRecipe(container, ASSEMBLER)`.
- **Expected**: Response returns `"gt:assembly:copper_rod"`.
- **Follow-up**: Machine service calls `Craft("gt:assembly:copper_rod", container)`.
- **Expected**: Response returns container with 1 copper rod in slot 0, 4 iron plates remain in slots 2-3.

#### Scenario 3: No recipe matches

- **Setup**: Furnace container with 1 unknown ore in slot 0.
- **Action**: SimulationCore calls `CheckRecipe(container, FURNACE)`.
- **Expected**: Response returns `null`.
- **Follow-up**: Machine service calls `Craft("gt:smelting:unknown", container)`.
- **Expected**: Response returns container unchanged (no crafting performed).

---

**Generated**: 2026-05-30 | **Branch**: main
