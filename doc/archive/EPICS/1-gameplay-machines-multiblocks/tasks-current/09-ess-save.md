# Task 9: Implement EntityStateStore Saving for Machine Entities

## Objective
Ensure that machine entities created in SimulationCore are properly saved to EntityStateStore for persistence.

## Requirements

### 9.1 Update SimulationEngine::createMachineEntity() to save to EntityStateStore
**Location**: `src/services/simulation_core/ECS/SimulationEngine.cpp`

**Implementation**:
```cpp
void SimulationEngine::createMachineEntity(const Vec3i& pos, 
                                          uint16_t blockId,
                                          const MachineInfo& info) {
    // Create machine entity with all required components
    entt::entity machineEntity = registry.create();
    registry.emplace<Position>(machineEntity, pos);
    registry.emplace<MachineComponent>(machineEntity, info);
    registry.emplace<EnergyStorage>(machineEntity, /* initialized from info */);
    registry.emplace<InventoryContainer>(machineEntity, /* initialized from info */);
    registry.emplace<RecipeProgress>(machineEntity, 0); // idle
    
    // Save to EntityStateStore
    EntityState state = getEntityState(machineEntity);
    entityStateStore->save(machineEntity, state);
    
    // Log creation
    logMachineCreation(pos, blockId);
}
```

### 9.2 Implement getEntityState() helper function
**Location**: `src/services/simulation_core/ECS/SimulationEngine.h` / `.cpp`

**Implementation**:
```cpp
// In header:
struct EntityState {
    Vec3i position;
    MachineComponent machine;
    EnergyStorage energy;
    InventoryContainer inventory;
    RecipeProgress recipe;
    // ... other components as needed
};

// In implementation:
EntityState SimulationEngine::getEntityState(entt::entity entity) {
    EntityState state;
    
    // Get position
    auto& pos = registry.get<Position>(entity);
    state.position = pos;
    
    // Get machine component
    if (registry.all_of<MachineComponent>(entity)) {
        state.machine = registry.get<MachineComponent>(entity);
    }
    
    // Get energy storage
    if (registry.all_of<EnergyStorage>(entity)) {
        state.energy = registry.get<EnergyStorage>(entity);
    }
    
    // Get inventory
    if (registry.all_of<InventoryContainer>(entity)) {
        state.inventory = registry.get<InventoryContainer>(entity);
    }
    
    // Get recipe progress
    if (registry.all_of<RecipeProgress>(entity)) {
        state.recipe = registry.get<RecipeProgress>(entity);
    }
    
    return state;
}
```

### 9.3 Update EntityStateStore interface
**Location**: `src/services/simulation_core/EntityStateStore.h` / `.cpp`

**Implementation** (ensure save method exists):
```cpp
// In EntityStateStore.h:
class EntityStateStore {
public:
    void save(entt::entity entity, const EntityState& state);
    EntityState load(entt::entity entity);
    void remove(entt::entity entity);
    // ... other methods
};
```

### 9.4 Implement EntityStateStore save method
**Location**: `src/services/simulation_core/EntityStateStore.cpp`

**Implementation**:
```cpp
void EntityStateStore::save(entt::entity entity, const EntityState& state) {
    // Convert entity to unique ID for persistence
    EntityId entityId = getEntityId(entity);
    
    // Serialize state to LMDB
    std::string key = std::to_string(entityId);
    std::string value = serializeEntityState(state);
    
    // Store in LMDB
    lmdbStore.put(DB_ENTITY_STATES, key, value);
    
    logInfo("Saved entity state for entity " + std::to_string(entityId));
}
```

### 9.5 Update SimulationEngine::onBlockChanged() to save machine entities
**Location**: `src/services/simulation_core/ECS/SimulationEngine.cpp`

**Implementation**:
```cpp
void SimulationEngine::onBlockChanged(const BlockChangedEvent& event) {
    uint16_t blockId = event.blockId;
    
    // Handle machine creation
    const MachineInfo* machineInfo = MachineRegistry::getMachineInfo(blockId);
    if (machineInfo != nullptr) {
        createMachineEntity(event.pos, blockId, *machineInfo);
        return; // Machine created, no other entities needed
    }
    
    // Handle other block types...
    // (existing logic for crafting_table, tools, etc.)
}
```

### 9.6 Add EntityStateStore loading to SimulationCore initialization
**Location**: `src/services/simulation_core/main.cpp`

**Implementation**:
```cpp
// In SimulationCore main initialization:
simulationCore->entityStateStore->loadAll(); // Load all persisted entities
```

### 9.7 Add EntityStateStore cleanup on machine destruction
**Location**: `src/services/simulation_core/ECS/SimulationEngine.cpp`

**Implementation**:
```cpp
// When a machine block is destroyed:
void SimulationEngine::destroyMachineEntity(const Vec3i& pos) {
    // Find machine entity at position
    auto view = registry.view<Position, MachineComponent>();
    for (auto entity : view) {
        if (view.get<Position>(entity) == pos) {
            // Remove from ECS
            registry.destroy(entity);
            
            // Remove from EntityStateStore
            EntityId entityId = getEntityId(entity);
            entityStateStore->remove(entityId);
            
            logInfo("Destroyed machine entity at " + pos.toString());
            break;
        }
    }
}
```

## Evidence Requirements
- [ ] createMachineEntity() calls entityStateStore->save()
- [ ] EntityState struct contains all machine components
- [ ] EntityStateStore has save() and remove() methods
- [ ] LMDB storage is working for entity states
- [ ] Machine entities are properly persisted
- [ ] Machine destruction removes entities from storage
- [ ] Entity loading on SimulationCore startup works

## Dependencies
- EntityStateStore implementation
- LMDB storage system
- Entity serialization/deserialization
- Entity ID system

## Testing
- Machine creation should persist state
- Server restart should reload machine states
- Machine destruction should clean up storage
- Multiple machines should be saved independently
- Entity state should be complete and recoverable

---