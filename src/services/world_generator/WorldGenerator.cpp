#include "WorldGenerator.h"

#include <array>
#include <cstdint>
#include <cmath>
#include <limits>
#include <glm/glm.hpp>
#include <glm/gtc/noise.hpp>

#include "FastNoise/FastNoise.h"
#include "OreConfig.h"
#include "OreGenerator.h"
namespace {
    thread_local FastNoise::SmartNode<FastNoise::Simplex> fnFractal_ = FastNoise::New<FastNoise::Simplex>();
}
void WorldGenerator::GenerateFlat(Chunk& c, int cx, int cy, int cz) {
    (void)cx; (void)cz;
    for(int y = 0; y < 32; y++) {
        int worldY = cy * 32 + y;
        uint16_t block = (worldY < 4)   ? 1 : // камень
                         (worldY == 4)  ? 2 : // трава
                         0;                    // воздух
        for(int z = 0; z < 32; z++) {
            for(int x = 0; x < 32; x++) {
                c.SetBlock(x, y, z, block);
            }
        }
    }
}

float WorldGenerator::GetTerrainHeight(int worldX, int worldZ) { //TODO - via fast noise
    // Scale coordinates for noise
    float scale = 0.02f;
    
    // Base terrain using Perlin noise
    float noise = glm::perlin(glm::vec2(worldX * scale, worldZ * scale));
    
    // Add detail with higher frequency noise
    noise += 0.5f * glm::perlin(glm::vec2(worldX * scale * 2.0f, worldZ * scale * 2.0f));
    noise += 0.25f * glm::perlin(glm::vec2(worldX * scale * 4.0f, worldZ * scale * 4.0f));
    
    // Normalize to height range where stone/dirt/grass layers are clearly visible
    float height = 10.0f + noise * 8.0f;
    
    return height;
}

float WorldGenerator::Get3DNoise(int worldX, int worldY, int worldZ) {
    float scale = 0.05f;
    return glm::perlin(glm::vec3(worldX * scale, worldY * scale, worldZ * scale));
}

void WorldGenerator::naiveGenerateNoise(const int baseX, const int baseZ, const int baseY, float(&caveNoise)[32][32][32], float(&oreNoise)[32][32][32]) {
    for (int x = 0; x < 32; ++x) {
        for (int y = 0; y < 32; ++y) {
            for (int z = 0; z < 32; ++z) {
                caveNoise[x][y][z] = Get3DNoise(baseX + x, baseY + y, baseZ + z);
            }
        }
    }

    for (int x = 0; x < 32; ++x) {
        for (int y = 0; y < 32; ++y) {
            for (int z = 0; z < 32; ++z) {
                oreNoise[x][y][z] = Get3DNoise((baseX + x) * 2, (baseY + y) * 2, (baseZ + z) * 2);
            }
        }
    }
}

void WorldGenerator::GenerateTerrain(Chunk& c, int cx, int cy, int cz) {
    const int baseX = cx * 32;
    const int baseZ = cz * 32;
    const int baseY = cy * 32;
    const auto seed = 100;

    float heights[32][32];
    for (int x = 0; x < 32; ++x) {
        for (int z = 0; z < 32; ++z) {
            heights[x][z] = GetTerrainHeight(baseX + x, baseZ + z);
        }
    }

    // 1) Высоты (2D) — 1024 вызова GetTerrainHeight
    /*float heights[1024];
    fractal->SetOctaveCount(3);
    fractal->SetLacunarity(2.0f);     // удвоение частоты на октаву
    fractal->SetGain(0.5f);           // уменьшение амплитуды вдвое

    fractal->GenUniformGrid2D(heights, baseX, baseZ, 32, 32, 0.02f, 0.02f, seed);
    std::transform(heights, heights + 1024, heights,
                   [](float v) { return 10.0f + v * 8.0f; });*/
    /*float caveNoise[32][32][32];
    float oreNoise[32][32][32];
    naiveGenerateNoise(baseX, baseZ, baseY, caveNoise, oreNoise);*/

    std::array<float, 32 * 32 * 32> caveNoise;
    fnFractal_->GenUniformGrid3D(caveNoise.data(), baseX, baseY, baseZ, 32, 32, 32, 0.05f, 0.05f, 0.05f, seed);
    std::array<float, 32 * 32 * 32> oreNoise;
    fnFractal_->GenUniformGrid3D(oreNoise.data(), baseX, baseY, baseZ, 32, 32, 32, 0.1f, 0.1f, 0.1f, seed);

        for (int y = 0; y < 32; ++y) {
            int worldY = baseY + y;
            for (int z = 0; z < 32; ++z) {
                int z_offset = z * 32;
                for (int x = 0; x < 32; ++x) {
                    float terrainHeight = heights[x][z];

                    uint16_t block = 0;
                    if (worldY < terrainHeight - 4) {
                        block = 1; // stone
                    } else if (worldY < terrainHeight - 1) {
                        block = 3; // dirt
                    } else if (worldY < terrainHeight) {
                        block = 2; // grass
                    } else if (worldY < 0) {
                        block = 4; // water
                    }

                    int idx = (z_offset + y) * 32 + x;

                    // Пещеры (caveNoise — плоский массив float)
                    if (block == 0 || block == 4) {
                        c.SetBlock(x, y, z, block);
                        continue;
                    }

                    if (caveNoise[idx] > 0.4f) {
                        block = 0;
                    }

                    c.SetBlock(x, y, z, block);
                }
            }
        }

        int32_t minTerrainHeight = std::numeric_limits<int32_t>::max();
        for (int x = 0; x < 32; ++x) {
            for (int z = 0; z < 32; ++z) {
                if (heights[x][z] < minTerrainHeight) {
                    minTerrainHeight = static_cast<int32_t>(heights[x][z]);
                }
            }
        }
        
        // Lazy-init ore configuration from JSON
        static bool oreConfigLoaded = false;
        if (!oreConfigLoaded) {
            OreConfig::instance().load("data/registry/ores.json");
            oreConfigLoaded = true;
        }
        
        OreGenerator oreGen(seed);
        oreGen.generateOres(cx, cz, minTerrainHeight, c.getBlocks());
}
