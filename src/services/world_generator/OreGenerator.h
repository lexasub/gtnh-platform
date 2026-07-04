#pragma once

#include "OreConfig.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

class OreGenerator {
public:
  explicit OreGenerator(int32_t worldSeed);

  void generateOres(int32_t chunkX, int32_t chunkZ, int32_t baseHeight,
                    std::array<uint16_t, 32 * 32 * 32> &blocks,
                    int32_t chunkSize = 32);

private:
  float calculateOreNoise(const OreDef &ore, int32_t worldX, int32_t worldY,
                          int32_t worldZ) const;
  bool isInHeightRange(const OreDef &ore, int32_t y) const;
  bool shouldGenerateInChunk(int32_t chunkX, int32_t chunkZ,
                             const OreDef &ore) const;
  float calculateDensityNoise(const OreDef &ore, int32_t worldX, int32_t worldY,
                              int32_t worldZ) const;

  int32_t m_worldSeed;
};