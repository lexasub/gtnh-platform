# Task 5: Integrate OreGenerator into WorldGenerator

## Objective
Replace the hardcoded `block_id=5` in `WorldGenerator::GenerateTerrain()` with the new OreGenerator system driven by `ores.json` configuration.

## Requirements

### 5.1 Update WorldGenerator.h
**Location**: `src/services/world_generator/WorldGenerator.h`

```cpp
#pragma once
#include "GenerationQueue.h"
#include "OreGenerator.h"  // NEW
#include <memory>

class WorldGenerator {
public:
    WorldGenerator(uint64_t seed);
    virtual ~WorldGenerator() = default;
    
    virtual void GenerateFlat(std::vector<uint16_t>& out, int32_t chunkX, int32_t chunkZ);
    virtual void GenerateTerrain(std::vector<uint16_t>& out, int32_t chunkX, int32_t chunkZ);
    
    // NEW:
    void setOreConfig(const std::string& configPath);
    
private:
    uint64_t m_seed;
    std::unique_ptr<OreGenerator> m_oreGen;  // NEW
};
```

### 5.2 Update WorldGenerator.cpp
**Location**: `src/services/world_generator/WorldGenerator.cpp`

**Current** (from explore agent):
```cpp
void WorldGenerator::GenerateTerrain(std::vector<uint16_t>& out, int32_t chunkX, int32_t chunkZ) {
    // ... terrain height generation ...
    // ... stone/grass/dirt layers ...
    
    // Lines 127-132: hardcoded ore
    auto oreNoise = FastNoise::NewFromEncodedNodeTree("FGN<Simplex,FBrown,...>");
    oreNoise->GenUniformGrid3D(oreNoiseData, chunkX*32, 0, chunkZ*32, 32, 256, 32, 0.1f, seed);
    if (block == 1) {
        if (oreNoise[idx] > 0.7f) {
            block = 5;  // gold_ore
        }
    }
}
```

**Replace with**:
```cpp
void WorldGenerator::GenerateTerrain(std::vector<uint16_t>& out, int32_t chunkX, int32_t chunkZ) {
    // ... existing terrain height generation (UNCHANGED) ...
    // ... existing stone/grass/dirt layers (UNCHANGED) ...
    
    // REMOVE lines 127-132 (hardcoded ore)
    
    // NEW: Apply ore generation on top of stone terrain
    if (m_oreGen) {
        // Calculate column height map (from existing terrain gen)
        // The m_heightMap or similar is needed — depends on existing code
        m_oreGen->generateOres(chunkX, chunkZ, terrainHeight, out);
    }
}
```

### 5.3 Update WorldGenerator constructor
```cpp
WorldGenerator::WorldGenerator(uint64_t seed) 
    : m_seed(seed)
{
    // Load ore config on construction
    auto& config = OreConfig::instance();
    config.load("data/registry/ores.json");
    m_oreGen = std::make_unique<OreGenerator>(seed);
}
```

### 5.4 Verify integration with CMakeLists.txt
**Location**: `src/services/world_generator/CMakeLists.txt`

Add new source files:
```cmake
# Existing: worldgeneratord library
add_library(worldgeneratord STATIC
    WorldGenerator.cpp
    GenerationQueue.cpp
    OreGenerator.cpp      # NEW
    OreConfig.cpp         # NEW
)
```

### 5.5 Verify terrain height access
**Critical**: `OreGenerator::generateOres()` needs terrain height per column to avoid generating above surface. The existing `WorldGenerator::GenerateTerrain()` must expose or store the height map.

If terrain height isn't stored:
```cpp
// Option A: store heightmap in WorldGenerator
std::vector<int32_t> m_columnHeights;  // 32x32 per chunk

// Option B: pass heightmap to OreGenerator
void generateOres(chunkX, chunkZ, const std::vector<int32_t>& heightMap, blocks);
```

## Acceptance Criteria
- [ ] Hardcoded `block_id=5` code removed from WorldGenerator.cpp
- [ ] OreGenerator called during terrain generation
- [ ] Terrain height map accessible to OreGenerator
- [ ] Ore blocks only appear below terrain surface
- [ ] Existing terrain generation (height, layers, caves) UNCHANGED
- [ ] OreConfig loads from `data/registry/ores.json` at startup
- [ ] Without ores.json → graceful fallback (no ore generation, no crash)
- [ ] CMakeLists.txt includes new files

## Dependencies
- Task 3 (OreGenerator class)
- Task 4 (height layers)
- Required by: Task 6 (mining), Task 7 (persistence)

## Files to Modify
- `src/services/world_generator/WorldGenerator.h` — add m_oreGen, setOreConfig()
- `src/services/world_generator/WorldGenerator.cpp` — replace block_id=5 ore code
- `src/services/world_generator/CMakeLists.txt` — add new source files
