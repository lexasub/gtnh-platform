# Task 12: Fluid Pipe Visuals (Client)

## Objective
Add client-side visuals for fluid pipes: pipe model tinted blue for water, colored for other fluids, with optional fluid level animation.

## Requirements

### 12.1 Fluid pipe mesh (extends Task 7)
**Location**: `src/services/game_client/rendering/PipeMeshBuilder.cpp`

Reuse the pipe mesh builder from Task 7 with fluid-specific variations:
```cpp
class PipeMeshBuilder {
    // Existing methods from Task 7...
    
    // NEW: fluid level indicator inside pipe
    void addFluidLevel(MeshData& mesh, FaceMask connections, uint16_t fluidItemId, uint8_t fillLevel);
};
```

### 12.2 Fluid color mapping
```cpp
// src/services/game_client/rendering/FluidColors.h
struct FluidColor {
    uint16_t itemId;
    glm::vec3 color;        // RGB
    float transparency;     // 0-1
};

const std::unordered_map<uint16_t, FluidColor> FLUID_COLORS = {
    {ITEM_ID_WATER,         {ITEM_ID_WATER,         {0.2f, 0.4f, 0.8f}, 0.3f}},
    {ITEM_ID_STEAM,         {ITEM_ID_STEAM,         {0.8f, 0.8f, 0.8f}, 0.1f}},
    {ITEM_ID_SULFURIC_ACID, {ITEM_ID_SULFURIC_ACID, {0.8f, 0.6f, 0.0f}, 0.4f}},
};
```

### 12.3 Fluid level animation
- Inner cylinder inside pipe mesh shows fluid level (0% to 100% filled)
- Level updates each tick based on `fluidBuffer.size() / fluidCapacity`
- Water = blue tinted, steam = white/transparent, acid = yellow

### 12.4 Pipe material assignment
**Location**: `src/services/game_client/rendering/MaterialRegistry.cpp`

```cpp
// Pipe materials
MaterialId MAT_PIPE_METAL;      // outer pipe casing
MaterialId MAT_PIPE_FLUID_GLASS; // transparent inner pipe
MaterialId MAT_FLUID_WATER;      // water color
MaterialId MAT_FLUID_STEAM;      // steam color
```

## Acceptance Criteria
- [ ] Fluid pipes show transparent inner cylinder with fluid color
- [ ] Water = blue, steam = white transparent, acid = yellow
- [ ] Fluid level changes as items flow through (animated)
- [ ] Metal pipe casing renders the same as item pipe
- [ ] Fluid visible through glass part of pipe
- [ ] Performance: fluid pipe mesh generation < 1ms per chunk

## Dependencies
- Task 7 (pipe mesh builder foundation)
- Task 8 (fluid item IDs — for color mapping)
- Task 9 (fluid graph — for fill level data)

## Files to Modify
- `src/services/game_client/rendering/PipeMeshBuilder.cpp` — fluid level geometry
- `src/services/game_client/rendering/FluidColors.h` — NEW: color definitions
- `src/services/game_client/rendering/MaterialRegistry.cpp` — fluid materials
