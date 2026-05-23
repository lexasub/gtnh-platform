# Task 10: Implement EntityStateStore Removal for Machine Destruction

## Objective
Ensure that machine entities are properly removed from EntityStateStore when the corresponding block is destroyed.

## Requirements

### 10.1 Update BlockEventHandler to detect machine destruction
**Location**: `src/services/simulation_core/EventListener/ChunkEventHandler.cpp`

**Implementation**:
```cpp
// In BlockEventHandler::onBlockChanged():
void ChunkEventHandler::onBlockChanged(const BlockChangedEvent& event) {
    uint16_t oldBlockId = event.oldBlockId; // Block that was replaced/destroyed
    uint16_t newBlockId = event.blockId;    // New block
    
    // Check if old block was a machine
    const MachineInfo* oldMachineInfo = MachineRegistry::getMachineInfo(oldBlockId);
    if (oldMachineInfo != nullptr) {
        // Old block was a machine - destroy its entity
        simulationEngine->destroyMachineEntity(event.pos, oldBlockId);
    }
    
    // Handle new block placement...
    // (existing logic for creating new entities)
}
```

### 10.2 Implement destroyMachineEntity() in SimulationEngine
**Location**: `src/services/simulation_core/ECS/SimulationEngine.cpp`

**Implementation**:
```cpp
void SimulationEngine::destroyMachineEntity(const Vec3i& pos, uint16_t blockId) {
    // Find machine entity at position
    auto view = registry.view<Position, MachineComponent>();
    for (auto entity : view) {
        if (view.get<Position>(entity) == pos) {
            // Remove from ECS
            registry.destroy(entity);
            
            // Remove from EntityStateStore
            EntityId entityId = getEntityId(entity);
            entityStateStore->remove(entityId);
            
            // Log destruction
            logMachineDestruction(pos, blockId, entityId);
            
            break;
        }
    }
}
```

### 10.3 Implement EntityStateStore::remove() method
**Location**: `src/services/simulation_core/EntityStateStore.cpp`

**Implementation**:
```cpp
void EntityStateStore::remove(entt::entity entity) {
    EntityId entityId = getEntityId(entity);
    std::string key = std::to_string(entityId);
    
    // Remove from LMDB
    lmdbStore.deleteKey(DB_ENTITY_STATES, key);
    
    logInfo("Removed entity state for entity " + std::to_string(entityId));
}

// Overloaded version with EntityId:
void EntityStateStore::remove(EntityId entityId) {
    std::string key = std::to_string(entityId);
    lmdbStore.deleteKey(DB_ENTITY_STATES, key);
}
```

### 10.4 Update ChunkEventHandler to handle all machine types (dynamic lookup)
**Location**: `src/services/simulation_core/EventListener/ChunkEventHandler.cpp`

**Implementation** (НОЛЬ хардкода — через MachineRegistry):
```cpp
void ChunkEventHandler::onBlockChanged(const BlockChangedEvent& event) {
    // Dynamic lookup — не хардкод!
    const MachineInfo* info = MachineRegistry::getMachineInfo(event.oldBlockId);
    if (info != nullptr) {
        simulationEngine->destroyMachineEntity(event.pos, event.oldBlockId);
    }
}
```
> ❗ **НЕ ИСПОЛЬЗОВАТЬ** хардкоженный список block_id. Это то, от чего мы уходим.
> Всегда использовать `MachineRegistry::getMachineInfo()` или `MachineRegistry::All()`.

### 10.5 Add validation to prevent double destruction
**Location**: `src/services/simulation_core/ECS/SimulationEngine.cpp`

**Implementation**:
```cpp
void SimulationEngine::destroyMachineEntity(const Vec3i& pos, uint16_t blockId) {
    // Find machine entity at position
    auto view = registry.view<Position, MachineComponent>();
    for (auto entity : view) {
        if (view.get<Position>(entity) == pos) {
            // Validate this is the correct machine type
            const MachineInfo* info = MachineRegistry::getMachineInfo(blockId);
            if (info == nullptr) {
                logError("Cannot destroy machine: block " + std::to_string(blockId) + 
                        " not in registry");
                break;
            }
            
            // Additional validation: check if entity has correct machine component
            auto& machineComp = view.get<MachineComponent>(entity);
            if (machineComp.id != blockId) {
                logError("Entity-machine mismatch at " + pos.toString());
                break;
            }
            
            // All validations passed - proceed with destruction
            registry.destroy(entity);
            entityStateStore->remove(getEntityId(entity));
            logMachineDestruction(pos, blockId, getEntityId(entity));
            
            break;
        }
    }
}
```

### 10.6 Add EntityStateStore cleanup logging
**Location**: `src/services/simulation_core/EntityStateStore.cpp`

**Implementation**:
```cpp
void EntityStateStore::remove(EntityId entityId) {
    std::string key = std::to_string(entityId);
    
    // Check if entity exists before deleting
    bool existed = lmdbStore.exists(DB_ENTITY_STATES, key);
    
    if (existed) {
        lmdbStore.deleteKey(DB_ENTITY_STATES, key);
        logInfo("Removed entity state for entity " + std::to_string(entityId));
    } else {
        logWarning("Attempted to remove non-existent entity state " + 
                  std::to_string(entityId));
    }
}
```

### 10.7 Add EntityStateStore initialization to handle missing entries
**Location**: `src/services/simulation_core/EntityStateStore.cpp`

**Implementation**:
```cpp
void EntityStateStore::remove(EntityId entityId) {
    std::string key = std::to_string(entityId);
    
    // Check if entity exists before deleting
    if (!lmdbStore.exists(DB_ENTITY_STATES, key)) {
        logWarning("Attempted to remove non-existent entity state " + 
                  std::to_string(entityId));
        return;
    }
    
    lmdbStore.deleteKey(DB_ENTITY_STATES, key);
    logInfo("Removed entity state for entity " + std::to_string(entityId));
}
```

## Evidence Requirements
- [ ] ChunkEventHandler detects machine block destruction
- [ ] destroyMachineEntity() removes from both ECS and EntityStateStore
- [ ] EntityStateStore.remove() deletes LMDB entries
- [ ] Validation prevents incorrect entity destruction
- [ ] Logging provides clear feedback for machine destruction
- [ ] All machine types are properly handled
- [ ] Non-machine blocks are not affected

## Dependencies
- MachineRegistry implementation (Task 4)
- EntityStateStore implementation (Task 9)
- ChunkEventHandler implementation
- SimulationEngine implementation

## Testing
- Machine destruction should clean up both ECS and storage
- EntityStateStore should be empty after all machines destroyed
- Server restart should not reload destroyed machines
- Multiple machines should be destroyed independently
- Non-machine blocks should not trigger machine destruction

---