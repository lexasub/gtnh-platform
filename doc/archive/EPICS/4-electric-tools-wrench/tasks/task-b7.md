# Task B7: Client Face Texture Update

## Objective
When a machine's side_config changes, update the client-side block face texture to reflect the new role (e.g., ENERGY face shows energy coil texture, OUTPUT face shows output arrow).

## Requirements

### 7.1 Face textures per role
**Location**: `src/services/game_client/assets/textures/`

```
machines/face_default.png     — Default machine casing (ANY role)
machines/face_input.png       — Green arrow IN
machines/face_output.png      — Red arrow OUT
machines/face_energy.png      — Blue energy coil
machines/face_fluid_in.png    — Cyan fluid in
machines/face_fluid_out.png   — Cyan fluid out
machines/face_none.png        — Dark/gray blocked face
```

### 7.2 Texture mapping
**Location**: `src/services/game_client/rendering/BlockTextureRegistry.cpp`

```cpp
// Map SideRole → texture index
const std::unordered_map<uint8_t, uint16_t> FACE_TEXTURES = {
    {0, TEXTURE_FACE_INPUT},      // INPUT
    {1, TEXTURE_FACE_OUTPUT},     // OUTPUT
    {2, TEXTURE_FACE_ENERGY},     // ENERGY
    {3, TEXTURE_FACE_FLUID_IN},   // FLUID_IN
    {4, TEXTURE_FACE_FLUID_OUT},  // FLUID_OUT
    {5, TEXTURE_FACE_DEFAULT},    // ANY
    {6, TEXTURE_FACE_NONE},       // NONE
};
```

### 7.3 Per-face texture in chunk mesh
**Location**: `src/services/game_client/rendering/ChunkMeshBuilder.cpp`

Machine blocks need per-face texture assignment (unlike most blocks which use 1 texture for all faces):

```cpp
if (isMachineBlock(blockId) && machineHasSideConfig(blockId)) {
    // Get stored side_config for this block
    // (received from server via machine.config.updated → cached in ClientWorld)
    auto* config = m_clientWorld->getMachineSideConfig(x, y, z);
    
    if (config) {
        for (int face = 0; face < 6; face++) {
            uint8_t role = config->faces[face];
            auto it = FACE_TEXTURES.find(role);
            if (it != FACE_TEXTURES.end()) {
                setFaceTexture(face, it->second);
            }
        }
    }
}
```

### 7.4 Server config cache on client
**Location**: `src/services/game_client/World/ClientWorld.h`

```cpp
struct MachineSideConfig {
    uint16_t machineId;
    uint8_t faces[6];
    uint64_t lastUpdated;
};

class ClientWorld {
    // ... existing ...
    
    // NEW:
    std::unordered_map<BlockPos, MachineSideConfig> m_machineSideConfigs;
    
    void onMachineConfigUpdated(const Protocol::MachineConfigUpdated* event);
    const MachineSideConfig* getMachineSideConfig(int32_t x, int32_t y, int32_t z) const;
};
```

### 7.5 Handle machine.config.updated on client
**Location**: `src/services/game_client/Network/NetClient.cpp`

```cpp
void NetClient::onMachineConfigUpdated(const std::vector<uint8_t>& data) {
    auto* event = flatbuffers::GetRoot<Protocol::MachineConfigUpdated>(data.data());
    
    // Update cache
    MachineSideConfig config;
    config.machineId = event->machine_id();
    for (int i = 0; i < 6 && i < event->faces()->size(); i++) {
        config.faces[i] = event->faces()->Get(i);
    }
    config.lastUpdated = event->timestamp();
    
    m_clientWorld->setMachineSideConfig(
        BlockPos{event->pos()->x(), event->pos()->y(), event->pos()->z()},
        config);
    
    // Mark chunk for re-mesh (face textures changed)
    m_world->markChunkDirty(event->pos()->x() / 32, event->pos()->z() / 32);
}
```

### 7.6 Chunk re-mesh on config change
When a machine's face textures change, the chunk needs to be re-meshed. Use existing chunk dirty marking system.

## Acceptance Criteria
- [ ] 7 face textures exist (default, input, output, energy, fluid_in, fluid_out, none)
- [ ] Machine face shows correct texture per role
- [ ] Default (ANY) shows standard machine casing
- [ ] INPUT face shows green arrow
- [ ] ENERGY face shows blue coil
- [ ] NONE face shows dark/gray blocked texture
- [ ] Texture updates when WRENCH_CYCLE succeeds
- [ ] Chunk re-meshes on config change
- [ ] Config survives chunk reload (cached from server state)

## Dependencies
- Task B2 (MachineAction protocol — CONFIG_UPDATED message)
- Task B6 (publish machine.config.updated)
- Existing chunk mesh builder
- Required by: (none — terminal in client visual chain)

## Files to Modify/Create
- `src/services/game_client/assets/textures/machines/` — NEW: face textures
- `src/services/game_client/rendering/BlockTextureRegistry.cpp` — texture mapping
- `src/services/game_client/rendering/ChunkMeshBuilder.cpp` — per-face textures
- `src/services/game_client/World/ClientWorld.h/.cpp` — config cache
- `src/services/game_client/Network/NetClient.cpp` — event handler
