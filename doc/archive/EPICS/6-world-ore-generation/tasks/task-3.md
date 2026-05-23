# Task 3: 3D Sinusoidal Ore Vein Generation

## Objective
Replace the hardcoded `gold_ore` check in WorldGenerator with proper 3D sinusoidal vein generation using FastNoiseLite, driven by OreConfig.

## Requirements

### 3.1 Create OreGenerator class
**Location**: `src/services/world_generator/OreGenerator.h/.cpp` (NEW)

```cpp
#pragma once
#include "OreConfig.h"
#include <FastNoise/FastNoise.h>
#include <vector>

class OreGenerator {
public:
    OreGenerator(int32_t worldSeed);
    
    // Generate ores for a chunk (32x32x32 block region)
    // Returns block_id for each position (0 = air/stone, >0 = ore)
    void generateOres(
        int32_t chunkX, int32_t chunkZ,
        int32_t baseHeight,  // terrain height at this column
        std::vector<uint16_t>& blocks,  // 32x32x32 output
        int32_t chunkSize = 32
    );
    
private:
    float calculateOreNoise(const OreDef& ore, 
                            int32_t worldX, int32_t worldY, int32_t worldZ) const;
    bool isInHeightRange(const OreDef& ore, int32_t y) const;
    
    int32_t m_worldSeed;
    // One noise generator per ore type for independent frequency control
    std::vector<FastNoise::SmartNode<>> m_noiseGenerators;
};
```

### 3.2 Sinusoidal vein algorithm
**Location**: `src/services/world_generator/OreGenerator.cpp`

```cpp
void OreGenerator::generateOres(int32_t chunkX, int32_t chunkZ,
                                int32_t baseHeight,
                                std::vector<uint16_t>& blocks,
                                int32_t chunkSize)
{
    auto& config = OreConfig::instance();
    int32_t baseX = chunkX * chunkSize;
    int32_t baseZ = chunkZ * chunkSize;
    
    for (const auto& ore : config.allOres()) {
        // Skip rare ores based on rarity roll
        if (ore.rarity < 1.0f) {
            // Use hash of chunk position + ore type for deterministic rarity
            if (!shouldGenerateInChunk(chunkX, chunkZ, ore)) continue;
        }
        
        for (int32_t x = 0; x < chunkSize; x++) {
            for (int32_t z = 0; z < chunkSize; z++) {
                int32_t worldX = baseX + x;
                int32_t worldZ = baseZ + z;
                
                // Only generate below terrain surface
                int32_t terrainHeight = getTerrainHeight(baseX + x, baseZ + z);
                
                for (int32_t y = ore.min_y; y <= std::min(ore.max_y, terrainHeight - 1); y++) {
                    if (y < 0) continue;
                    
                    int idx = (y * chunkSize + z) * chunkSize + x;
                    if (blocks[idx] != 1) continue;  // only replace stone
                    
                    float noise = calculateOreNoise(ore, worldX, y, worldZ);
                    
                    if (noise > ore.threshold) {
                        // Vein density check
                        float densityNoise = calculateDensityNoise(ore, worldX, y, worldZ);
                        if (densityNoise > (1.0f - ore.density)) {
                            blocks[idx] = ore.block_id;
                        }
                    }
                }
            }
        }
    }
}
```

### 3.3 Noise calculation
```cpp
float OreGenerator::calculateOreNoise(const OreDef& ore,
                                       int32_t worldX, int32_t worldY, int32_t worldZ) const
{
    // 3D sinusoidal noise: sin(ax) * sin(by) * sin(cz)
    // Creates vein-like structures instead of scattered blobs
    float fx = worldX * ore.frequency + m_worldSeed * 0.1f;
    float fy = worldY * ore.frequency + m_worldSeed * 0.13f;
    float fz = worldZ * ore.frequency + m_worldSeed * 0.17f;
    
    return std::sin(fx) * std::sin(fy) * std::sin(fz);
}
```

**Alternative**: Use FastNoiseLite Simplex with FractalBrownianMotion:
```cpp
// If sinusoidal doesn't produce good results, use FBM Simplex:
// (requires setting up noise generator per ore type)
float OreGenerator::calculateOreNoise_FBM(const OreDef& ore,
                                           int32_t worldX, int32_t worldY, int32_t worldZ) const
{
    return m_fbmNoise->GenSingle3D(worldX, worldY, worldZ, m_worldSeed);
}
```

### 3.4 Integration with existing FastNoise
**Current code** (WorldGenerator.cpp:127-132):
```cpp
auto oreNoise = FastNoise::NewFromEncodedNodeTree("FGN<Simplex,FBrown,Pos16,Pos16,Pos16,Pos16,Pos16,Pos16>");
oreNoise->GenUniformGrid3D(oreNoiseData, chunkX*32+chunkOffset, 0, chunkZ*32+chunkOffset, 32, 256, 32, 0.1f, seed);
if (block == 1) {
    if (oreNoise[idx] > 0.7f) block = 5;
}
```

**Replace with**:
```cpp
// New flow:
OreGenerator oreGen(seed);
oreGen.generateOres(chunkX, chunkZ, terrainHeight, chunkData);
```

## Acceptance Criteria
- [ ] `OreGenerator` class created
- [ ] Sinusoidal vein algorithm produces realistic vein shapes
- [ ] Noise value > threshold → potential ore placement
- [ ] Density check filters to ~30-60% of noise-pass blocks
- [ ] Only replaces stone blocks (block_id=1), not air or other blocks
- [ ] Doesn't generate above terrain surface
- [ ] Rarity system: diamond/lapis appear in ~20-50% of chunks
- [ ] Existing flat world generation still works (non-ore blocks unaffected)

## Dependencies
- Task 1 (ore item IDs)
- Task 2 (ore config — OreDef parameters)
- Required by: Task 4 (height layers), Task 5 (WorldGenerator integration)

## Files to Create/Modify
- `src/services/world_generator/OreGenerator.h` — NEW
- `src/services/world_generator/OreGenerator.cpp` — NEW
