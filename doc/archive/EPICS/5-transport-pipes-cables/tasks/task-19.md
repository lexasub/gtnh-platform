# Task 19: Client Cable Visuals + Explosion Effect

## Objective
Add client-side visuals for cables (wire model, tier-based colors) and explosion effects when cables overheat.

## Requirements

### 19.1 Cable block model
**Location**: `src/services/game_client/assets/models/` or `rendering/CableMeshBuilder.h` (NEW)

Cable model = thin wire through center of block:
- Cable occupies center ~25% of block volume (not full cube like pipes)
- Connections to adjacent cables form continuous wire
- No connection = small termination point

```cpp
class CableMeshBuilder {
public:
    MeshData buildCableMesh(int32_t x, int32_t y, int32_t z, const CableDef& def, FaceMask connections);
    
private:
    void addWireSegment(MeshData& mesh, FaceMask connections);
    void addTermination(MeshData& mesh, Face face);
};
```

### 19.2 Tier-based cable colors
**Location**: `src/services/game_client/rendering/CableColors.h` (NEW)

```cpp
const std::unordered_map<uint8_t, glm::vec3> CABLE_COLORS = {
    {1, {0.72f, 0.45f, 0.20f}},  // LV: copper/brown
    {2, {0.85f, 0.65f, 0.13f}},  // MV: gold/yellow
    {3, {0.40f, 0.40f, 0.40f}},  // HV: tungsten/gray
    {4, {0.60f, 0.60f, 0.80f}},  // EV: platinum/silver-blue
};
```

### 19.3 Cable connection detection (reuse from Task 7)
Same logic as pipe connections: check 6 neighbors for same cable type → FaceMask.

### 19.4 Overheat visual effects
**Location**: `src/services/game_client/rendering/ParticleSystem.cpp` or similar

When `cable.explosion` event received:
```cpp
void spawnCableExplosion(int32_t x, int32_t y, int32_t z, float radius) {
    // 1. Screen shake: 4 pixels, 0.3s duration
    // 2. Particles: 50 sparks, random directions, 0.5s life
    // 3. Flash: white sphere flash, 0.1s
    // 4. Block destruction animation (block → fragments → disappear)
}

void spawnOverheatSparks(int32_t x, int32_t y, int32_t z) {
    // While isOverheating == true:
    // 1. 2-3 sparks per tick from cable surface
    // 2. Spark color = cable color + white tint
    // 3. Spark life: 0.3s
    // 4. Sound: electrical buzz (spatial audio)
}
```

### 19.5 Material registration
**Location**: `src/services/game_client/rendering/MaterialRegistry.cpp`

```cpp
MaterialId MAT_CABLE_COPPER;
MaterialId MAT_CABLE_GOLD;
MaterialId MAT_CABLE_TUNGSTEN;
MaterialId MAT_CABLE_PLATINUM;
MaterialId MAT_TRANSFORMER_CASING;
MaterialId MAT_TRANSFORMER_HIGH_SIDE;
```

## Acceptance Criteria
- [ ] Cables render as thin wires (not full blocks)
- [ ] Cable connections between adjacent cables → continuous wire
- [ ] Cable color matches tier: copper=LV, gold=MV, gray=HV, silver=EV
- [ ] Explosion: screen shake, sparks, flash, block fragments
- [ ] Overheat: sparks at cable position while overheating
- [ ] Cables clickable (raycast works on thin geometry or block bounds)
- [ ] Transformer renders with high-side-face marker

## Dependencies
- Task 7 (pipe mesh builder — can reuse connection detection)
- Task 14 (cable block IDs — CableDef for colors)
- Task 16 (overheat → explosion event)

## Files to Create/Modify
- `src/services/game_client/rendering/CableMeshBuilder.h/.cpp` — NEW
- `src/services/game_client/rendering/CableColors.h` — NEW
- `src/services/game_client/rendering/ParticleSystem.cpp` — explosion particles
- `src/services/game_client/rendering/MaterialRegistry.cpp` — cable materials
