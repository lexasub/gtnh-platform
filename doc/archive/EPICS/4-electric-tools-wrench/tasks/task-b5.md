# Task B5: side_config Persistence via EntityStateStore

## Objective
Save and restore machine side_config through EntityStateStore so machine face configurations survive service restarts.

## Requirements

### 5.1 SideConfig FlatBuffers schema
**Location**: `src/protocol/entity_state_store.fbs`

```flatbuffers
table SideConfigData {
    machine_instance_id: uint64;
    faces: [uint8];            // 6 uint8 values (0-6)
    last_modified: uint64;     // timestamp
}
```

### 5.2 Save on role change
**Location**: `src/services/simulation_core/Tools/WrenchHandler.cpp` (extending Task B4)

```cpp
void WrenchHandler::saveSideConfig(entt::entity entity, const MachineComponent& machine) {
    // Save to EntityStateStore
    flatbuffers::FlatBufferBuilder builder(64);
    
    auto facesVec = builder.CreateVector(machine.side_config, 6);
    auto data = Protocol::CreateSideConfigData(builder, 
        machine.machine_instance_id, facesVec, getCurrentTimestamp());
    builder.Finish(data);
    
    // Publish to entity.state.set
    EntityStateStoreClient::save("machine_side_config_" + std::to_string(machine.machine_instance_id),
                                  builder.GetBufferPointer(), builder.GetSize());
}
```

### 5.3 Load on startup
**Location**: `src/services/simulation_core/ECS/Systems/MachineSystem.cpp` or `SimulationEngine.cpp`

When a machine entity is created (on chunk load), restore side_config:

```cpp
void SimulationEngine::onMachineEntityCreated(entt::entity entity, MachineComponent& machine) {
    // Restore side config from ESS
    auto data = EntityStateStoreClient::load(
        "machine_side_config_" + std::to_string(machine.machine_instance_id));
    
    if (data.found) {
        auto* sideConfig = flatbuffers::GetRoot<Protocol::SideConfigData>(data.data());
        for (int i = 0; i < 6 && i < sideConfig->faces()->size(); i++) {
            machine.side_config[i] = sideConfig->faces()->Get(i);
        }
    }
    // If not found, use defaults (all ANY)
}
```

### 5.4 Save key format
Key = `"machine_side_config_" + machine_instance_id`

Example: `machine_side_config_42` → [5,5,0,1,2,5]

### 5.5 EntityStateStore key cleanup on machine destruction
When a machine block is broken:
```cpp
// In dissociation or block break handler
EntityStateStoreClient::remove("machine_side_config_" + std::to_string(machine.machine_instance_id));
```

## Acceptance Criteria
- [ ] `SideConfigData` FlatBuffers table defined
- [ ] Side config saved to ESS on every WRENCH_CYCLE
- [ ] Side config restored from ESS when chunk loads
- [ ] Missing key → default (all ANY, no error)
- [ ] Machine destruction → key removed from ESS
- [ ] Save only when role actually changes (avoid redundant writes)
- [ ] No blocking on save (async if possible)

## Dependencies
- Task B1 (side_config in MachineComponent)
- Task B4 (wrench cycle handler — triggers save)
- EntityStateStore service (already exists)

## Files to Modify
- `src/protocol/entity_state_store.fbs` — SideConfigData
- `src/services/simulation_core/Tools/WrenchHandler.cpp` — save on cycle
- `src/services/simulation_core/ECS/SimulationEngine.cpp` — restore on entity create
- `src/services/simulation_core/ECS/Systems/MachineSystem.cpp` — cleanup on destroy
