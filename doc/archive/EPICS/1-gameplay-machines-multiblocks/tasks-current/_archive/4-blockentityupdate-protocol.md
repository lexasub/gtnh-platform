# TASK: BlockEntityUpdate Protocol
**Layer**: 1
**Status**: ✅ **DONE** — FlatBuffers `BlockEntityUpdate` table in core.fbs. Published by SimulationCore, consumed by GameClient/MachineWindow + RecipeManager.
**Epic**: 1-gameplay-machines-multiblocks

## Affected Services

| Service | Role | Direction |
|---------|------|-----------|
| **Protocol** | FlatBuffer schema — `BlockEntityUpdate` table | — |
| **SimulationCore** | Source — emits BlockEntityUpdate on machine state change | → |
| **Gateway** | Relay — forwards to interested clients | → |
| **GameClient** | Consumer — applies to GUI slots and progress bar | ← |

---

## Overview

`BlockEntityUpdate` synchronizes machine state from SimulationCore to GameClient via Gateway. It carries the complete state of a single machine at a given moment: inventory, processing progress, and energy level.

This protocol enables the client GUI to display real-time machine behavior without requiring a full chunk retransmission. Machines tick at 20 Hz (50 ms intervals), so updates are sent at most every 50 ms.

---

## FlatBuffer Schema Definition

```flatbuffers
namespace Protocol {

table MachineState {
  recipe_id: uint16;
  progress: float;
  max_progress: float;
  is_processing: bool;
};

table MachineInventory {
  input_slots: [Slot];
  output_slots: [Slot];
};

table Slot {
  item_id: uint16;
  count: uint8;
};

table EnergyStorage {
  capacity: uint32;
  current: uint32;
};

table BlockEntityUpdate {
  x: uint32;
  y: uint32;
  z: uint32;
  machine_type: uint16;
  machine_state: MachineState;
  machine_inventory: MachineInventory;
  energy_storage: EnergyStorage;
};

}  // namespace Protocol
```

---

## Message Flow

```
SimulationCore
   │
   │ (20 Hz tick → on state change)
   │ emits BlockEntityUpdate
   │
   ▼
Gateway (asio coroutine)
   │
   │ (zero-copy FlatBuffer serialization)
   │ sends ChunkData/EntitySnap
   │
   ▼
GameClient (bgfx + ImGui)
   │
   │ (decodes FlatBuffer)
   │ updates GUI window
   │
   ▼
```

---

## Field Descriptions

### Coordinates
- `x, y, z` — Chunk-local coordinates of the machine block. Used as the key for GUI updates.
- Range: 0..15 for each dimension (standard 16×16×16 chunk size).

### Machine Type
- `machine_type` — Block ID of the machine.
- Known IDs: `FURNACE`, `MACERATOR`, `COMPRESSOR`.
- Used to select the correct GUI template (slot layout, progress bar style).

### MachineState
- `recipe_id` — 0 = idle / no recipe; 1..N = active recipe selected.
- `progress` — Current progress in the recipe (0.0 to 1.0).
- `max_progress` — Total ticks required for the recipe.
- `is_processing` — Convenience flag; true if `progress > 0.0`.

### MachineInventory
- `input_slots` — Array of input slots. Size varies by machine type (furnace = 2, macerator = 2, compressor = 2).
- `output_slots` — Array of output slots. Currently size 1 for all machines.
- Each `Slot` holds `item_id` (block ID) and `count` (stack size, 0..64).

### EnergyStorage
- `capacity` — Total energy the machine can store (in EU/tick units).
- `current` — Energy currently stored.
- Machines consume energy per tick when running a recipe. If `current < recipe.energy_cost`, processing pauses until recharged.

---

## File Locations

| Artifact | Location |
|----------|----------|
| FlatBuffer schema | `src/protocol/BlockEntityUpdate.fbs` |
| C++ bindings (generated) | `src/protocol/BlockEntityUpdate_generated.h` |
| SimulationCore tick logic | `src/services/simulation_core/TileEntity.cpp` |
| Gateway serialization | `src/services/gateway/ChunkData.cpp` |
| GameClient GUI update | `src/services/game_client/Gui/Window.cpp` |

---

## Acceptance Criteria

#### Scenario: Furnace processes ore
1. Player places FURNACE block and adds coal and iron ore to input slots.
2. MachineState.is_processing becomes true, progress advances 1/200 per tick.
3. After 200 ticks, output slot contains 1 ingot, input slots empty.
4. GUI shows progress bar filling, then completes with output item visible.

#### Scenario: Energy depletion
1. Machine has EnergyStorage.capacity = 100, current = 10.
2. Recipe requires 5 EU per tick and 200 ticks duration.
3. Machine runs for 2 ticks, then pauses when current < 5.
4. GUI progress bar stops advancing until external power recharges energy.

#### Scenario: Multiple machines in same chunk
1. Player places FURNACE at (10, 64, 10) and MACERATOR at (12, 64, 10).
2. Both machines receive separate BlockEntityUpdate messages with distinct coordinates.
3. GUI displays two separate windows, each showing correct inventory and progress for its machine.

---

**Generated**: 2026-05-23 | **Branch**: main
