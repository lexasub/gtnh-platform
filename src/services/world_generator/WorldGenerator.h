#pragma once

#include "../chunk_store/Chunk/Chunk.h"

class WorldGenerator {
public:
    virtual ~WorldGenerator() = default;

    // Generates a flat world: y=0..3 stone, y=4 grass, y>4 air
    void GenerateFlat(Chunk& c, int cx, int cy, int cz);
    
    // Generates 3D terrain with hills using Perlin noise
    void GenerateTerrain(Chunk& c, int cx, int cy, int cz);

private:
    void naiveGenerateNoise(const int baseX, const int baseZ, const int baseY, float(&caveNoise)[32][32][32], float(&oreNoise)[32][32][32]);
    // Simple 2D noise for terrain height
    float GetTerrainHeight(int worldX, int worldZ);
    
    // 3D noise for caves and ore generation
    float Get3DNoise(int worldX, int worldY, int worldZ);
};
