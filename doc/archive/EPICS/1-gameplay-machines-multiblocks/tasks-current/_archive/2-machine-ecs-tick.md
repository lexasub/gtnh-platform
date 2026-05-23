# Machine ECS Components & Tick System

**Layer**: 1  
**Status**: 🟡 **Implemented** — MachineSystem 3-pass tick (2026-06-20). Single-block machines: passive/processing states, energy consumption, recipe check via RecipeManager. Multiblocks: `managed_externally` flag → reciped via `world.block_entity.update`.  
**Epic**: 1-gameplay-machines-multiblocks

## Affected Services

| Service | Role | R/W |
|---------|------|-----|
| **SimulationCore** | Primary — owns EnTT ECS, runs 20 Hz tick, MachineState/MachineInventory/EnergyStorage components | Read/Write |
| **RecipeManager** ⬅️ NEW | Dependency — called via RPC for CheckRecipe on idle, Craft on completion | Read |
| **EntityStateStore** ⬅️ NEW | Persistence — saves/loads machine state on chunk events | Write |
| **PipeNetwork** | Dependency — energy consumption checked per tick | Read |
| **MessageRouter** | Transport — dispatches RPCs | — |

> **Architecture rule**: SimulationCore calls RecipeManager/EntityStateStore/PipeNetwork as a client over MessageRouter. These services are **not** routed through SimulationCore — they are independently accessible.

---

## Overview

Machines are blocks that process items over time or using energy. The architecture treats machines as regular blocks with an attached `TileEntity` stored in ECS `SimulationCore`. All machines tick at 20 Hz.

This task specifies the ECS components and the tick loop that drives machine behavior. Components are EnTT components attached to machine entities in SimulationCore.

---

## ECS Components

### MachineState

Holds the current recipe and processing progress.

```cpp
struct MachineState {
    uint16_t recipe_id;      // 0 = no recipe active
    float progress;          // 0.0 to 1.0
    float max_progress;      // recipe duration in ticks
    bool is_processing;      // convenience flag
};
```

### Inventory

Simple slot-based inventory with input and output slots.

```cpp
struct MachineInventory {
    struct Slot {
        uint16_t item_id;
        uint8_t count;
    };

    Slot input_slots[2];   // configurable per machine
    Slot output_slots[1];  // single output slot
};
```

### EnergyStorage

Basic energy buffer. Machines initially run on magic (infinite energy) — this component provides the foundation for future energy networks.

```cpp
struct EnergyStorage {
    uint32_t capacity;     // EU/tick
    uint32_t current;      // EU currently stored
};
```

### Processing

Handles recipe matching and progression.

```cpp
struct Processing {
    struct Recipe {
        uint16_t id;
        std::array<uint16_t, 2> input_ids;
        std::array<uint16_t, 1> output_ids;
        uint32_t energy_cost; // EU per tick
        uint32_t duration_ticks;
    };

    Recipe get_by_id(uint16_t id);
    bool match(const MachineInventory& inv) const;
};
```

---

## Tick Loop Logic

20 Hz global tick. Each machine executes the following steps:

1. **Recipe selection** — if idle, query `RecipeManager` with input inventory
2. **Energy check** — verify `current_energy >= recipe.energy_cost`
3. **Progress** — `progress += 1 / duration_ticks`
4. **Completion** — when `progress >= 1.0`:
   - Remove input items
   - Add output items
   - Reset `progress = 0.0`
5. **Sync** — emit update with new state

---

## Energy Handling

Machines initially run on magic (infinite energy). The `EnergyStorage` component serves as the foundation for future energy networks. A machine processes if `energy >= cost_per_tick`.

---

## Sync Mechanism

Machine state must be synced to the client: progress bar, inventory, energy. Uses `EntitySnapshot` with extended fields (or separate `BlockEntityUpdate`).

---

## File Locations

- Components: `src/services/simulation_core/include/components/machine.hpp`
- Tick logic: `src/services/simulation_core/src/machine_tick.cpp`
- Recipe manager: `src/services/recipe_manager/` (RecipeManager.cpp)

---

## Acceptance Criteria

#### Scenario: Recipe selection

Given a machine with empty input slots
When the machine ticks and no recipe is active
Then the machine queries the RecipeManager with its input inventory
And returns no recipe selected
And remains idle

Given a machine with items that match a known recipe
When the machine ticks and no recipe is active
Then the machine queries the RecipeManager with its input inventory
And returns a valid recipe
And sets `recipe_id` to the matched recipe

#### Scenario: Energy check

Given a machine with a selected recipe that requires 5 EU per tick
When the machine has 10 EU stored and ticks
Then the energy check passes
And the machine advances progress

Given a machine with a selected recipe that requires 10 EU per tick
When the machine has 3 EU stored and ticks
Then the energy check fails
And the machine does not advance progress
And remains stuck until energy is supplied

#### Scenario: Progress advancement

Given a machine with a recipe that takes 100 ticks to complete
When the machine ticks
Then `progress` increases by 0.01
And `max_progress` is set to 100
And `is_processing` is set to true

Given a machine with `progress = 50.0` and a recipe that takes 100 ticks
When the machine ticks
Then `progress` increases by 0.01 to 51.0
And the machine continues processing

#### Scenario: Completion

Given a machine with `progress = 99.0` and a recipe that takes 100 ticks
When the machine ticks
Then `progress` increases by 0.01 to 100.0
And the input items are consumed
And the output items are placed in the output slot
And `progress` is reset to 0.0
And `is_processing` is set to false

#### Scenario: Multiple machines

Given the world contains three machines: a furnace, a macerator, and a compressor
When all machines tick simultaneously at 20 Hz
Then each machine processes independently
And progress updates occur at exactly 20 Hz per machine
And no machines interfere with each other

#### Scenario: Recipe change during processing

Given a machine with `progress = 0.5` on a recipe that takes 100 ticks
When the input items are replaced with items that match a different recipe
And the new recipe takes 50 ticks
Then the machine abandons the current recipe
And selects the new recipe
And `progress` is reset to 0.0
And `max_progress` is updated to 50

---