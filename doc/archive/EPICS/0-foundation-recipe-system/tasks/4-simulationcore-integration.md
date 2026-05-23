# SimulationCore Recipe Integration

**Layer**: 1  
**Status**: Draft  
**Epic**: 0-foundation-recipe-system

## Affected Services

| Service | Role | R/W |
|---------|------|-----|
| **SimulationCore** | Primary — calls CheckRecipe/Craft during 20 Hz tick | Read/Write |
| **RecipeManager** ⬅️ NEW | Dependency — validates and executes recipes | Read/Write |
| **Gateway** | Relay — forwards machine state to/from client | Relay |
| **GameClient** | Consumer — displays machine progress | Read |
| **MessageRouter** | Transport — dispatches all RPCs | — |

> **Architecture rule**: **SimulationCore is NOT a proxy/gateway to RecipeManager.** Client/Gateway can call RecipeManager directly via MessageRouter (e.g., to list recipes in a GUI). SimulationCore is just one consumer among equals. RecipeManager, EntityStateStore, and PipeNetwork are all first-class services on the message bus.

## Overview

SimulationCore executes the 20 Hz machine tick loop across all machines in the world. This layer defines how SimulationCore interacts with RecipeManager to validate and execute recipes during its tick cycle.

RecipeManager is accessed via RPC calls over MessageRouter — independently, not through SimulationCore. Each machine instance maintains its own `MachineInventory`, which holds input and output slots. The integration points occur at two moments: (1) when the machine is idle and needs to determine if a recipe applies, and (2) when a recipe completes and needs to craft its outputs.

---

## Integration Points

### CheckRecipe on Idle Machine

When a machine enters an idle state, SimulationCore invokes `CheckRecipe` with the current `MachineInventory` and the machine's type.

```cpp
// Pseudocode in SimulationCore
bool Machine::isIdle() {
    return !isProcessing() && !hasPendingInput() && !hasPendingOutput();
}

void Machine::onIdle() {
    RecipeManager::CheckRecipe(container, type);
}
```

The `CheckRecipe` RPC returns a `recipe_id` if a matching recipe exists, or null if no recipe applies.

### Craft on Completion

When a recipe finishes processing, SimulationCore invokes `Craft` with the `recipe_id` and the current container state.

```cpp
void Machine::onRecipeComplete() {
    RecipeManager::Craft(recipe_id, container);
}
```

The `Craft` RPC returns the updated container with new items added to the output slots.

---

## Tick Loop Flow

Each machine follows this per-tick sequence:

```
[Start Tick]
    │
    ▼
┌─────────────┐
│ Is machine  │
│ processing? │
└──────┬──────┘
       │
       ▼ No
┌─────────────┐
│ CheckRecipe │ ──► recipe_id = null ──► Machine stays idle
│ (idle state)│
└──────┬──────┘
       │
       ▼ recipe_id found
┌─────────────┐
│ Process 1/  │
│ duration    │
│ ticks       │
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ Did it      │ ──► No ──► Continue ticking
│ complete?   │
└──────┬──────┘
       │
       ▼ Yes
┌─────────────┐
│ Craft(recipe│ ──► new_container ──► Update inventory
│ _id,         │
│ container)  │
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ Machine     │
│ idle        │
└──────┬──────┘
```

---

## Error Handling

### Recipe Not Found

If `CheckRecipe` returns null, the machine remains idle until the container changes. No error is logged since this is normal behavior.

```cpp
RecipeId recipe_id = RecipeManager::CheckRecipe(container, type);
if (!recipe_id) {
    // Normal case: no recipe applies. Stay idle.
    return;
}
```

### Craft Fails

If `Craft` returns an error, SimulationCore logs the failure, keeps the original items in the container, and does not consume any input.

```cpp
ContainerResult result = RecipeManager::Craft(recipe_id, container);
if (result.hasError()) {
    spdlog::error("Craft failed for recipe {} with container {}", recipe_id, container.state());
    // Keep original items. Do not consume inputs.
    continue;
}
container = result.container;
```

---

## File Locations

- **Specification**: `doc/EPICS/0-foundation-recipe-system/tasks/4-simulationcore-integration.md`
- **Implementation stubs**: `src/src/services/simulation_core/machines/`
- **RecipeManager RPC interface**: `src/src/services/protocol/`
- **MachineInventory**: `src/src/services/simulation_core/machines/MachineInventory.hpp`

---

## Acceptance Criteria

#### Scenario: Idle machine with valid recipe

- **Given**: A furnace is idle with 1 coal and 1 iron ore in the input slot
- **When**: SimulationCore runs the 20 Hz tick
- **Then**: `CheckRecipe` returns a valid recipe_id
- **And**: The machine begins processing for the recipe duration
- **And**: Each tick reduces the recipe progress by 1

#### Scenario: Idle machine with no matching recipe

- **Given**: A furnace is idle with 1 copper block in the input slot
- **When**: SimulationCore runs the 20 Hz tick
- **Then**: `CheckRecipe` returns null
- **And**: The machine stays idle
- **And**: The copper block remains in the input slot

#### Scenario: Recipe completion with Craft failure

- **Given**: A furnace has an active recipe that completes at tick 50
- **When**: `Craft` is invoked but fails due to insufficient energy
- **Then**: An error is logged
- **And**: The iron ore remains in the output slot
- **And**: The coal remains in the input slot
- **And**: The machine resets to idle state

#### Scenario: Successful Craft update

- **Given**: A furnace completes a smelting recipe
- **When**: `Craft` is invoked successfully
- **Then**: The furnace returns the updated container with 1 iron ingot in the output slot
- **And**: The machine resets to idle state
- **And**: `CheckRecipe` is called again on the next idle tick

---

## Open Questions

1. **Container ownership** — Does SimulationCore own the container copy, or must RecipeManager return a new container each time?
2. **Recipe caching** — Should SimulationCore cache recipe_id results to reduce RPC latency during continuous processing?
3. **Energy validation** — Should `CheckRecipe` return early if the machine lacks sufficient energy, or should this be handled in `Craft`?
4. **Partial output** — What happens if `Craft` adds outputs to a full inventory slot? Should it drop the item, return an error, or shift existing items?

---

**Generated**: 2026-06-01 | **Branch**: main
