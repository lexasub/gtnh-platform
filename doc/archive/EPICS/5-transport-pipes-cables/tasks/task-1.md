# Task 1: Add Pipe Block IDs & isPipeBlock()

## Objective
Add item_pipe, fluid_pipe, dense_item_pipe, dense_fluid_pipe block IDs to the system and implement `isPipeBlock()` in PipeNetworkService to recognize actual pipe blocks.

## Requirements

### 1.1 Register pipe block IDs in data/registry
**Location**: `data/registry/items.csv` + `data/registry/consumers.csv` (pipe blocks are infrastructure, not machines)

Add new entries:
```
item_pipe, TBD, infrastructure
fluid_pipe, TBD, infrastructure
dense_item_pipe, TBD, infrastructure
dense_fluid_pipe, TBD, infrastructure
```

**Details**:
- Assign unique block_id values (next available after existing blocks, e.g., 64-67)
- `items.csv` format: `name, id, type` (verify exact format from existing entries)
- Pipe blocks do NOT go in consumers.csv/producers.csv — they are not machines

### 1.2 Add block_id constants or enum
**Location**: `src/services/pipe_network/PipeNetworkService.h` (or a new `PipeBlockIds.h`)

```cpp
// Existing: NO pipe block definitions exist
// Add:
constexpr uint16_t BLOCK_ID_ITEM_PIPE       = 64;
constexpr uint16_t BLOCK_ID_FLUID_PIPE      = 65;
constexpr uint16_t BLOCK_ID_DENSE_ITEM_PIPE = 66;
constexpr uint16_t BLOCK_ID_DENSE_FLUID_PIPE= 67;
```

### 1.3 Implement isPipeBlock()
**Location**: `src/services/pipe_network/PipeNetworkService.h` / `PipeNetworkService.cpp`

**Current (from explore agent)**:
```cpp
// PipeNetworkService.h — existing
bool isPipeBlock(uint16_t block_id) const;

// PipeNetworkService.cpp — current stub (from agent findings)
bool PipeNetworkService::isPipeBlock(uint16_t block_id) const {
    return false;  // <-- STUB: no pipe blocks recognized
}
```

**Replace with**:
```cpp
bool PipeNetworkService::isPipeBlock(uint16_t block_id) const {
    switch (block_id) {
        case BLOCK_ID_ITEM_PIPE:
        case BLOCK_ID_FLUID_PIPE:
        case BLOCK_ID_DENSE_ITEM_PIPE:
        case BLOCK_ID_DENSE_FLUID_PIPE:
            return true;
        default:
            return false;
    }
}
```

### 1.4 Register pipe block IDs in simulation_core
**Location**: `src/services/simulation_core/ECS/Systems/MachineSystem.cpp` or `SimulationEngine.cpp`

Pipe blocks should NOT get ECS Machine entities. Add pipe block_id to the skip list in machine entity creation logic (in `onBlockChanged` or wherever `isMachineBlock()` is checked).

### 1.5 Test pipe block registration
**Location**: N/A — manual verification via `isPipeBlock()` returning true for new IDs.

## Acceptance Criteria
- [ ] Block IDs defined as constants (header or enum)
- [ ] `isPipeBlock(64)` = true, `isPipeBlock(65)` = true, `isPipeBlock(0)` = false
- [ ] Pipe blocks do NOT create ECS machine entities
- [ ] Pipe blocks registered in items.csv
- [ ] No compilation errors

## Dependencies
- None — pipe block IDs are standalone
- Required by: Task 2 (item graph), Task 9 (fluid graph), Task 14 (cables)

## Files to Modify
- `data/registry/items.csv` — add pipe block entries
- `src/services/pipe_network/PipeNetworkService.h` — add block_id constants + isPipeBlock() declaration
- `src/services/pipe_network/PipeNetworkService.cpp` — implement isPipeBlock()
- `src/services/simulation_core/ECS/SimulationEngine.cpp` — skip pipe blocks in machine creation
