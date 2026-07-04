#pragma once

#include "../chunk_store/Chunk/Chunk.h"

class WorldGenerator {
public:
  virtual ~WorldGenerator() = default;

  // Generates 3D terrain with hills using Perlin noise
  void GenerateTerrain(Chunk &c, int cx, int cy, int cz);

private:
  // Simple 2D noise for terrain height
  float GetTerrainHeight(int worldX, int worldZ);
};
