# EPIC: Machines & Multiblocks

**Layer 1** + **Layer 2**  
**Status**: L1 🟡 (ECS tick + GUI + multiblock→reciped ✅), L2 🔴 deferred

## Affected Services

| Service | Layer | Role |
|---------|-------|------|
| **SimulationCore** | L1 | Primary — ECS, 20 Hz machine tick, MachineState/Inventory/EnergyStorage components, multiblock detection |
| **RecipeManager** ⬅️ **NEW** | L0 | Dependency — machine recipe validation via CheckRecipe/Craft |
| **EntityStateStore** ⬅️ **NEW** | L0 | Persistence — save/load machine TileEntity state |
| **ChunkStore** | L0 | Dependency — block storage, mb_id writes for multiblocks |
| **SpatialIndex** | **L2** | R-tree queries for multiblock pattern matching (не нужен на L1) |
| **PipeNetwork** | L1 | Dependency — energy consumption, flow graphs (отдельный сервис) |
| **Gateway** | L0 | Relay — forwards BlockEntityUpdate, PlayerAction |
| **GameClient** | L1 | Consumer — machine GUI, progress bars |

> **Architecture rule**: RecipeManager, EntityStateStore, PipeNetwork are first-class MessageRouter peers. SimulationCore is **not** a proxy — every service is independently callable. SpatialIndex — deferred до L2 (multiblocks).

---

## Overview

Machines are blocks that process items over time or using energy. The architecture treats machines as regular blocks with an attached `TileEntity` stored in ECS `SimulationCore`. All machines tick at 20 Hz.

---

## Simple Machines (MVP)

### What

Single-block machines: furnace, macerator, compressor. Each processes items based on time or energy.

### Architecture

- Machine is a block with a `TileEntity` (stored in ECS `SimulationCore`)
- Components: `MachineState`, `Inventory`, `EnergyStorage`, `Processing`
- When energy is supplied and items are present, processing starts
- Each tick (20 Hz) `SimulationCore` advances progress; on completion, input is replaced by output
- Machine state must be synced to the client: progress bar, energy. Uses `EntitySnapshot` with extended fields (or separate `BlockEntityUpdate`)

### Required Block IDs

- `FURNACE` — ore → ingot
- `MACERATOR` — ore → 2 dust
- `COMPRESSOR` — 2 dust → 1 plate

### Architecture (Detailed)

Machine state:
- Inventory (input/output slots)
- Energy buffer
- Current recipe
- Processing progress

Each tick (20 Hz), `SimulationCore` for every loaded machine:
1. If no recipe selected, request from `RecipeManager` using input inventory
2. If recipe exists, check if enough energy (if recipe requires), start/continue progress
3. On completion, consume input items, place outputs in output slot

Energy:
- Initially machines "run on magic" (infinite energy)
- Alternatively, a simple solar generator can provide 1 EU/tick
- No EU network implemented
- Machine has `EnergyStorage` component and runs if `energy >= cost_per_tick`

### Required Deliverables

- Define several block IDs: `FURNACE`, `MACERATOR`, `COMPRESSOR`
- Add recipes in `RecipeManager`
- Add TileEntity tick system in `SimulationCore` (generalizable for any machines)
- Add machine GUI in client (window with two slots and progress bar)

---

## Machine Components

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

Simple slot-based inventory with input and output.

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

Basic energy buffer.

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

## Tick Logic

20 Hz global tick. Each machine:

1. **Recipe selection** — if idle, query `RecipeManager` with input inventory
2. **Energy check** — verify `current_energy >= recipe.energy_cost`
3. **Progress** — `progress += 1 / duration_ticks`
4. **Completion** — when `progress >= 1.0`:
   - Remove input items
   - Add output items
   - Reset `progress = 0.0`
5. **Sync** — emit `BlockEntityUpdate` with new state

---

## Client GUI

GUI-спецификация машины описана в [0-basic-mechanics/basic-mechanics.md](../0-basic-mechanics/basic-mechanics.md) (секция 5 — UI компоненты, секция 7 — BlockEntityUpdate).

Детали реализации машинного GUI:
- Два input слота (слева)
- Один output слот (справа)
- Прогресс-бар (0–100%)
- Обновления через `BlockEntityUpdate`

---

## Multiblocks — Stage 2 — deferred

Multi-block structures: multiple blocks forming a single machine (blast furnace, steam boiler).

### Mechanism

`SimulationCore` detects pattern, creates `MultiblockController`, manages multiblock as unified entity.

### Required Primitives

- Input/output slots at various positions
- Shared inventory and energy storage
- Special rendering (custom mesh or bounding box)

### Protocol

Add `MultiblockStatus` message:
- Active multiblocks and their parts
- Inventory and energy state
- Overall progress

---

## EntityStateStore

**Planned C++ service** — `TileEntity` state storage by coordinates. RPC: `GetState`, `SetState`.

Used for storing machine inventories and workbench states. Key: `dim|x|y|z` → blob.

Separate from `ChunkStore` to preserve the principle of dumb storage.

```cpp
struct EntityStateStore {
    std::unordered_map<uint64_t, std::vector<uint8_t>> data;

    std::vector<uint8_t> get(uint32_t dim, uint32_t x, uint32_t y, uint32_t z) const;
    void set(uint32_t dim, uint32_t x, uint32_t y, uint32_t z, const std::vector<uint8_t>& data);
};
```

---

## Осталось реализовать (перенесено из archive/1-player-crafting)

- [x] **ConditionEvaluator с реальным MachineState** — ✅ уже реализован. `evaluateConditions(reg, ...)` перегрузка в `RecipeManager.cpp` заполняет MachineState из ECS (temperature, purity, biome, energy, network_ids, tags). Единственный gap — fluid_slots, но это решено решением "fluids as items".

## Previous Work (Archived)

See `../archive/1-gameplay-machines-multiblocks/` for completed tasks:
- ECS MachineSystem (20Hz tick, recipe matching, energy consumption, progress)
- Multiblock detection (matchElectrolyser, registerController)
- BlockEntityUpdate protocol — FlatBuffers table with hatches/covers/fluids
- ConditionEvaluator MachineState — populated from ECS via RecipeManager.cpp

## Completed (2026-06-20)

- [x] **Machine GUI** — `MachineWindow::Render()` data-driven from `MachineRegistry` (input/output slots, progress bar, energy bar) ✅
- [x] **MachineSystem refactoring** — 3-pass tick (Pass 0: reciped publishing, Pass 1: local recipe start, Pass 2: tick+energy). `managed_externally` flag for multiblock routing.
- [x] **Multiblock→reciped flow** — `SimulationEngine::onMachineCreated` callback → `world.block_entity.update` → reciped service. Hash-gated inventory publishing prevents infinite loops.
- [x] **BlockEntityUpdate protocol** — FlatBuffers table with `pos`, `machine_type`, `progress`, `energy`, `input_items`, `output_items`, `mb_id`, `structure_valid` (published by SimulationCore, consumed by RecipeManager)
- [x] **EntityStateStore** — C++ service with LMDB, TCP :5200, `entity.state.get/set` pub/sub. WorkbenchStateManager uses it for grid persistence.
- [x] **ConditionEvaluator with real MachineState** — `evaluateConditions(reg, ...)` populates from ECS (temperature, purity, biome, energy, network_ids)
- [x] **heat_generator → heat_furnace dogfooding (2026-06-20):**
  - [x] **EnergyType из MachineRegistry** — `SimulationEngine::onBlockChanged` читает `energy_in`/`energy_out`, heat-машины получают `EnergyType::HEAT`
  - [x] **HeatTransferSystem** — adjacency heat transfer (6 dirs, producer→consumer, MachineSystem пропускает PipeNetwork для HEAT)
  - [x] **BlockEntityUpdate: energy_capacity** — публикуется в FlatBuffer, MachineWindow отрисовывает шкалу
  - [x] **Client routing** — отдельный `kBlockEntityUpdate` коллбэк, MachineWindow парсит FlatBuffer вместо 8-байтового формата
  - [x] **Client MachineRegistry Init** — `GameClient::Init()` загружает CSV + `BlockUIFactory::LoadFromRegistry` (все машины кликабельны)
  - [x] **FuelValues расширение** — oak_planks (2000), stick (500) к coal (8000)

## Remaining Work

### L1 — `./l1-completion.md`

- [ ] **Machine slot interaction** — клиент → сервер: положить/забрать предметы в слоты машины. `InventoryAction` не умеет в контейнер/позицию — нужен новый протокол (`SetMachineSlotReq` с x/y/z/player_id/slot_idx/item_id/count/meta) (L1)
- [ ] **Server registration** — `SetBlockAction` → SimulationCore → create ECS entity with `MachineComponent` (L1)

### L2 — `doc/EPICS/7-multiblocks-l2/7-multiblocks-l2.md`

- [ ] **Multiblocks L2** — full gameplay logic: SpatialIndex, EBF, Large Boiler, LCR, dissociation, persistence
