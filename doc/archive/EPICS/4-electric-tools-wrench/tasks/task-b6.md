# Task B6: Publish machine.config.updated

## Objective
When a machine's side_config changes, publish a `machine.config.updated` event via MessageRouter so PipeNetwork and other services can react.

## Requirements

### 6.1 Config update event
**Location**: `src/protocol/simcore.fbs` or `pipe_network.fbs`

```flatbuffers
table MachineConfigUpdated {
    pos: Vec3i;
    machine_id: uint16;
    machine_instance_id: uint64;
    faces: [uint8];            // All 6 face roles
    changed_face: uint8;       // Which face changed
    old_role: uint8;
    new_role: uint8;
    timestamp: uint64;
}
```

### 6.2 Publish on role change
**Location**: `src/services/simulation_core/Tools/WrenchHandler.cpp` (extending Task B4)

```cpp
void WrenchHandler::publishConfigUpdated(int32_t x, int32_t y, int32_t z,
                                          const uint8_t sideConfig[6],
                                          uint8_t changedFace, uint8_t newRole,
                                          uint8_t oldRole, uint16_t machineId,
                                          uint64_t instanceId)
{
    flatbuffers::FlatBuilder builder(128);
    
    auto pos = Protocol::CreateVec3i(builder, x, y, z);
    auto faces = builder.CreateVector(sideConfig, 6);
    auto event = Protocol::CreateMachineConfigUpdated(builder, pos, machineId, 
        instanceId, faces, changedFace, oldRole, newRole, getCurrentTimestamp());
    builder.Finish(event);
    
    // Publish to "machine.config.updated" topic
    m_routerClient->publish("machine.config.updated", 
                            builder.GetBufferPointer(), builder.GetSize());
}
```

### 6.3 Subscribers
- **PipeNetwork** — when config changes, re-route energy/fluid/item connections
- **ChunkStore** — if needed for rendering or caching
- **Client** — forward config changes for visual updates (Task B7)
- **Gateway** — forward to connected clients
- **Monitoring/debug** — log config changes

### 6.4 Gateway forwarding
**Location**: `src/services/gateway/`

```cpp
routerClient->subscribe("machine.config.updated", [this](const Message& msg) {
    // Forward to all connected clients (or specific player)
    broadcastToClients(msg.data, msg.size, "MachineConfigUpdated");
});
```

### 6.5 Topic naming convention
Follow existing pattern: `machine.config.updated`

Existing topics for reference:
- `world.blocks.changed`
- `energy.node.update`
- `energy.consume.request/response`
- `energy.flow`
- `player.inventory.update`

## Acceptance Criteria
- [ ] `MachineConfigUpdated` FlatBuffers table defined
- [ ] Published on every successful WRENCH_CYCLE
- [ ] Topic: `machine.config.updated`
- [ ] Includes: pos, machine_id, all faces, changed face, old/new role
- [ ] Gateway forwards to connected clients
- [ ] PipeNetwork can subscribe and react (→ Task B8)
- [ ] No publish on failed cycle (no change)

## Dependencies
- Task B4 (wrench handler — triggers publish)
- MessageRouter infrastructure
- Required by: B7 (client visuals), B8 (PipeNetwork routing)

## Files to Modify
- `src/protocol/simcore.fbs` or `pipe_network.fbs` — MachineConfigUpdated
- `src/services/simulation_core/Tools/WrenchHandler.cpp` — publish call
- `src/services/gateway/` — forward to clients
