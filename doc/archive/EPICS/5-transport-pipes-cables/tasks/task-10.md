# Task 10: BoilerSystem → Pipe → Machine Flow

## Objective
Connect BoilerSystem (steam production) and steam-consuming machines through the fluid pipe network. Boiler produces `steam` item_id, machines consume it.

## Requirements

### 10.1 Update BoilerSystem to produce steam items
**Location**: `src/services/simulation_core/ECS/Systems/BoilerSystem.cpp`

**Current (from explore agent)** — BoilerSystem publishes `energy.node.update` with STEAM energy type.

**Change**: In addition to (or instead of) energy publishing, BoilerSystem should push `steam` items to an adjacent fluid pipe:

```cpp
void BoilerSystem::tick(entt::registry& registry, float dt) {
    // ... existing boiler logic (fuel burning, heat calculation) ...
    
    // NEW: Push steam to connected fluid pipe
    for (auto [entity, boiler, pos, inventory] : registry.view<BoilerComponent, Position, InventoryContainer>().each()) {
        if (boiler.steamProduced > 0) {
            // Check for adjacent fluid_pipe block
            for (int face = 0; face < 6; face++) {
                BlockPos neighbor = pos + faceOffset[face];
                uint16_t neighborBlockId = getBlockId(neighbor);
                
                if (isFluidPipe(neighborBlockId)) {
                    // Push steam item to pipe network
                    ItemSlot steamItem = {ITEM_ID_STEAM, static_cast<uint8_t>(boiler.steamProduced)};
                    m_pipeItemClient->pushItem(pos.x, pos.y, pos.z, steamItem.item_id, steamItem.count, face);
                    boiler.steamProduced -= steamItem.count;
                    break;
                }
            }
        }
    }
}
```

### 10.2 Update steam-consuming machines
**Location**: `src/services/simulation_core/ECS/Systems/MachineSystem.cpp`

Machines with energy_type=STEAM should accept steam items from pipes:

```cpp
void MachineSystem::tick(entt::registry& registry, float dt) {
    // ... existing energy consumption ...
    
    // NEW: Steammachines — accept steam from fluid pipe
    for (auto [entity, machine, pos, energy, inv] : 
         registry.view<MachineComponent, Position, EnergyStorage, InventoryContainer>().each()) {
        
        if (energy.energy_type == EnergyType::STEAM /* or ENERGY_LABEL_STEAM */) {
            // Check if steam arrives from pipe this tick
            auto steamItem = m_pipeItemClient->collectIncoming(pos.x, pos.y, pos.z);
            if (steamItem.item_id == ITEM_ID_STEAM) {
                // Convert steam to energy
                energy.addEnergy(steamItem.count * STEAM_TO_EU_RATIO);
                // Steam consumed — remove from pipe buffer
            }
        }
    }
}
```

### 10.3 Steam-to-energy conversion rate
**Location**: `src/services/simulation_core/ECS/Systems/BoilerSystem.h` or `MachineSystem.h`

```cpp
constexpr float STEAM_TO_EU_RATIO = 0.5f;  // 1 steam item = 0.5 EU
```

### 10.4 PipeNetwork registration for steam machines
**Location**: `src/services/pipe_network/PipeNetworkService.cpp`

Steam machines register as fluid sources/sinks:
- Boiler → `isFluidSource = true`, `fluidItemId = ITEM_ID_STEAM`
- Steam consumer → `isFluidSink = true`, `acceptedFluids = {ITEM_ID_STEAM}`

### 10.5 FlatBuffers — steam flow integration
**Location**: `src/protocol/pipe_network.fbs` or `simcore.fbs`

```flatbuffers
// Extend EnergyNodeUpdate for steam-specific fields
table SteamFlowData {
    steam_produced: uint32;       // boiler output
    steam_consumed: uint32;       // machine input
    conversion_rate: float;       // steam → EU
}
```

## Boiler → Steam Machine Flow
```
BoilerSystem                    PipeNetwork                    MachineSystem
    │                              │                              │
    ├─ tick()                      │                              │
    ├─ burn fuel                   │                              │
    ├─ produce steam item          │                              │
    ├─ pushItem(steam) ───────────►├─ findRoute()                 │
    │                              ├─ advanceItems()              │
    │                              ├─ deliverItem() ─────────────►├─ collectIncoming()
    │                              │                              ├─ convert steam→EU
    │                              │                              └─ run recipe
    └─ continue                   └─ continue                    └─ continue
```

## Acceptance Criteria
- [ ] BoilerSystem pushes `steam` items to adjacent fluid pipe
- [ ] Steam items travel through fluid pipe network (1 block/tick)
- [ ] Steam-consuming machines accept steam items from pipe
- [ ] `STEAM_TO_EU_RATIO` conversion works
- [ ] If no fluid pipe connected, steam is lost (or stored in boiler buffer)
- [ ] If machine is full, steam stays in pipe (backpressure)
- [ ] No regression in energy-based boiler operation

## Dependencies
- Task 8 (fluid item IDs — steam)
- Task 9 (fluid graph — pipe transport)
- Required by: none (terminal task in steam chain)

## Files to Modify
- `src/services/simulation_core/ECS/Systems/BoilerSystem.cpp` — push steam items
- `src/services/simulation_core/ECS/Systems/MachineSystem.cpp` — accept steam items
- `src/services/simulation_core/Network/PipeItemClient.h/.cpp` — collectIncoming() method
