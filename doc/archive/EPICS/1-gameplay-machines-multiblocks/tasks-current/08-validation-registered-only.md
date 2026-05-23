# Task 8: Implement Validation for Registered Machines Only

## Objective
Ensure that only block IDs registered in MachineRegistry receive MachineComponent, InventoryContainer, EnergyStorage, and RecipeProgress entities.

## Requirements

### 8.1 Update SimulationEngine::onBlockChanged() validation
**Location**: `src/services/simulation_core/ECS/SimulationEngine.cpp`

**Current Implementation** (to be fixed):
```cpp
void SimulationEngine::onBlockChanged(const BlockChangedEvent& event) {
    uint16_t blockId = event.blockId;
    
    // OLD (problematic):
    if (isMachineBlock(blockId)) {
        createMachineEntity(event.pos, blockId);
    }
    
    // ... other entity creation logic
}
```

**Required Implementation**:
```cpp
void SimulationEngine::onBlockChanged(const BlockChangedEvent& event) {
    uint16_t blockId = event.blockId;
    
    // NEW: Only registered machines get ECS entities
    const MachineInfo* machineInfo = MachineRegistry::getMachineInfo(blockId);
    if (machineInfo != nullptr) {
        createMachineEntity(event.pos, blockId, *machineInfo);
    } else {
        // For non-machine blocks, create appropriate entities
        // (e.g., crafting_table might need different components)
        createOtherBlockEntity(event.pos, blockId);
    }
    
    // ... other entity creation logic
}
```

### 8.2 Update createMachineEntity() to require validation
**Location**: `src/services/simulation_core/ECS/SimulationEngine.cpp`

**Implementation**:
```cpp
void SimulationEngine::createMachineEntity(const Vec3i& pos, 
                                          uint16_t blockId,
                                          const MachineInfo& info) {
    // Validate that this is indeed a machine
    if (MachineRegistry::getMachineInfo(blockId) == nullptr) {
        logError("Attempted to create machine entity for unregistered block " + 
                std::to_string(blockId));
        return;
    }
    
    // Continue with entity creation...
    // (This check is redundant but provides safety)
}
```

### 8.3 Update isMachineBlock() to be strict
**Location**: `src/services/simulation_core/ECS/SimulationEngine.cpp`

**Implementation** (after Task 4):
```cpp
// After Task 4 fix, isMachineBlock() should be:
bool isMachineBlock(uint16_t id) {
    return MachineRegistry::getMachineInfo(id) != nullptr;
}
```

This means:
- Registered machines: heat_furnace(36), heat_generator(46), etc. → true
- Non-machines: crafting_table(14), iron_pickaxe(35), chest(37), tools → false

### 8.4 Update createOtherBlockEntity() for non-machines
**Location**: `src/services/simulation_core/ECS/SimulationEngine.cpp`

**Implementation** (handle crafting_table, tools, etc.):
```cpp
void SimulationEngine::createOtherBlockEntity(const Vec3i& pos, uint16_t blockId) {
    // Different block types get different components
    switch (blockId) {
        case 14: // crafting_table
            // Create crafting table components (no MachineComponent)
            createCraftingTableEntity(pos);
            break;
        case 35: // iron_pickaxe
            // Create item entity (not a block)
            createItemEntity(pos);
            break;
        case 37: // chest
            // Create chest components (InventoryContainer but no MachineComponent)
            createChestEntity(pos);
            break;
        // ... other non-machine blocks
        default:
            // Default: just Position component
            auto entity = registry.create();
            registry.emplace<Position>(entity, pos);
            break;
    }
}
```

### 8.5 Update MachineComponent creation to require validation
**Location**: `src/services/simulation_core/ECS/SimulationEngine.cpp`

**Implementation**:
```cpp
void SimulationEngine::createMachineEntity(const Vec3i& pos, 
                                          uint16_t blockId,
                                          const MachineInfo& info) {
    // Pre-condition validation (should always pass after Tasks 4 & 8)
    if (MachineRegistry::getMachineInfo(blockId) == nullptr) {
        logError("Machine creation validation failed for block " + 
                std::to_string(blockId));
        return;
    }
    
    // Create machine entity with all required components
    entt::entity machineEntity = registry.create();
    registry.emplace<Position>(machineEntity, pos);
    registry.emplace<MachineComponent>(machineEntity, info);
    registry.emplace<EnergyStorage>(machineEntity, /* initialized from info */);
    registry.emplace<InventoryContainer>(machineEntity, /* initialized from info */);
    registry.emplace<RecipeProgress>(machineEntity, 0); // idle
    
    // Save to EntityStateStore
    entityStateStore->save(machineEntity, getEntityState(machineEntity));
}
```

### 8.6 Add validation logging
**Location**: `src/services/simulation_core/ECS/SimulationEngine.cpp`

**Implementation**:
```cpp
void SimulationEngine::logMachineCreation(const Vec3i& pos, uint16_t blockId) {
    const MachineInfo* info = MachineRegistry::getMachineInfo(blockId);
    if (info != nullptr) {
        logInfo("Created machine entity: " + info->name + 
               " at " + pos.toString() + 
               " (ID: " + std::to_string(blockId) + ")");
    } else {
        logWarning("Attempted to create non-machine entity at " + 
                  pos.toString() + " with block ID " + 
                  std::to_string(blockId));
    }
}
```

## Evidence Requirements
- [ ] isMachineBlock() returns true only for registered machines
- [ ] Non-machines (crafting_table, tools, chest) do NOT get MachineComponent
- [ ] SimulationEngine creates entities only for registered machines
- [ ] createMachineEntity() validates blockId before creation
- [ ] Validation logging provides clear feedback
- [ ] Non-machine blocks get appropriate alternative entities

## Dependencies
- MachineRegistry implementation (Task 4)
- isMachineBlock() fix (Task 4)
- createOtherBlockEntity() implementation
- Validation logging system

## Testing
- Registered machines should receive all 4 components (MachineComponent, EnergyStorage, InventoryContainer, RecipeProgress)
- Non-machines should NOT receive MachineComponent
- Crafting table should get crafting-specific components
- Iron pickaxe should get item-specific components
- Chest should get inventory components (no MachineComponent)
- Validation should catch and log incorrect machine creation attempts

---