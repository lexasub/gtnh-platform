# Task 7: Client Pipe Model + Connection Visuals

## Objective
Add visual representation of item pipes in the game client: 3D pipe models with connection-based rendering (connected pipes join visually, disconnected ends show flange).

## Requirements

### 7.1 Pipe block model
**Location**: `src/services/game_client/assets/models/`

Create or reference pipe model geometry:
- Central pipe segment (default)
- End caps for disconnected faces
- Connection flanges for connected faces
- Color/material: gray metal for basic item pipe, tinted for dense

**Approach**: Programmatic geometry (not a model file) — pipe is simple enough:
- 1×1×1 block with holes on connected faces
- If neighbor is same pipe type → full connection (no cap)
- If neighbor is not pipe → flange + hole

### 7.2 Pipe mesh generation
**Location**: `src/services/game_client/rendering/ChunkMeshBuilder.cpp` or `PipeMeshBuilder.h`

```cpp
class PipeMeshBuilder {
public:
    // Generate pipe mesh for a block position
    MeshData buildPipeMesh(int32_t x, int32_t y, int32_t z, PipeType type);
    
    // Check neighbors for connection
    bool isConnectedTo(int32_t x, int32_t y, int32_t z, Face face);
    
private:
    void addPipeSegment(MeshData& mesh, FaceMask connections);
    void addFlange(MeshData& mesh, Face face);
    void addEndCap(MeshData& mesh, Face face);
};

enum class PipeType : uint8_t {
    ITEM_PIPE,
    DENSE_ITEM_PIPE,
    FLUID_PIPE,
    DENSE_FLUID_PIPE,
    CABLE_TIN,
    CABLE_COPPER,
    // ...
};
```

### 7.3 Block registration for pipes in client
**Location**: `src/services/game_client/world/BlockRegistry.cpp` or similar

```cpp
// Register pipe blocks with custom mesh renderer instead of full-block mesh
void registerPipeBlocks() {
    for each pipe block_id:
        blockDef.renderType = BLOCK_RENDER_PIPE;  // custom renderer
        blockDef.pipeType = PipeType::ITEM_PIPE;   // mesh variant
}
```

### 7.4 Connection detection
Each frame or on chunk load:
1. For each pipe block, check 6 neighbors
2. If neighbor is same pipe type → connected face
3. Build `FaceMask` (6 bits: DOWN, UP, NORTH, SOUTH, WEST, EAST)
4. Pass mask to mesh builder

```cpp
FaceMask PipeMeshBuilder::detectConnections(int32_t x, int32_t y, int32_t z, PipeType type) {
    FaceMask mask = 0;
    // Check each neighbor
    if (getBlock(x, y-1, z) == pipeTypeToBlockId(type)) mask |= FACE_DOWN;
    if (getBlock(x, y+1, z) == pipeTypeToBlockId(type)) mask |= FACE_UP;
    // ... etc
    return mask;
}
```

### 7.5 Integration with existing chunk meshing
**Location**: `src/services/game_client/rendering/ChunkMeshBuilder.cpp`

Extend the chunk mesh generation to handle `BLOCK_RENDER_PIPE`:
```cpp
if (blockDef.renderType == BLOCK_RENDER_PIPE) {
    auto pipeMesh = pipeMeshBuilder.buildPipeMesh(x, y, z, blockDef.pipeType);
    chunkMesh.merge(pipeMesh);
}
```

## Acceptance Criteria
- [ ] Pipe blocks render as tubes instead of full cubes
- [ ] Connected faces show open pipe (connected to neighbor)
- [ ] Disconnected faces show flange/end cap
- [ ] Dense pipes different color/shape from basic
- [ ] Neighbor detection works across chunk boundaries
- [ ] Performance: pipe meshing doesn't increase chunk build time >10%
- [ ] Pipe blocks work with existing block interaction (raycast, placement)

## Dependencies
- Task 1 (pipe block IDs — needed for connection detection)
- GameClient rendering system (existing chunk mesh building)

## Files to Modify
- `src/services/game_client/` — new PipeMeshBuilder files
- `src/services/game_client/rendering/ChunkMeshBuilder.cpp` — pipe mesh integration
- `src/services/game_client/world/BlockRegistry.cpp` — pipe block render type
