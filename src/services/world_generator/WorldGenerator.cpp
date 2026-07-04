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
    thread_local FastNoise::SmartNode<FastNoise::Perlin> fnPerlin_ = FastNoise::New<FastNoise::Perlin>();
    thread_local FastNoise::SmartNode<FastNoise::Simplex> fnSimplex_ = FastNoise::New<FastNoise::Simplex>();
    thread_local FastNoise::SmartNode<FastNoise::FractalFBm> fbm = FastNoise::New<FastNoise::FractalFBm>();
    thread_local std::array<float, 32 * 32> heights;
    thread_local std::array<float, 32 * 32 * 32> caveNoise;
    /*
    * Биомы: Добавить fnBiome_ (низкочастотный шум) для смены типов поверхности (трава/песок/снег)
    * Деревья/растительность: Генерация на поверхности после основного террейна
    */
    thread_local bool initialized = false;

    void init() {
        if (initialized) return;
        fnPerlin_->SetScale(25.0f);
        fbm->SetSource(fnPerlin_);
        fbm->SetOctaveCount(3);
        fbm->SetLacunarity(2.0f);
        fbm->SetGain(0.5f);
        fnSimplex_->SetScale(35.0f);
        initialized = true;
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
    //float noise = fbm->GenSingle2D((float)worldX, (float)worldZ, 100);
    
    // Normalize to height range where stone/dirt/grass layers are clearly visible
    float height = 10.0f + noise * 8.0f;
    
    return height;
}

void WorldGenerator::GenerateTerrain(Chunk& c, int cx, int cy, int cz) {
    const int baseX = cx * 32;
    const int baseZ = cz * 32;
    const int baseY = cy * 32;
    const auto seed = 100;
    init();

    /*for (int x = 0; x < 32; ++x) {
        for (int z = 0; z < 32; ++z) {
            heights[z * 32 + x] = GetTerrainHeight(baseX + x, baseZ + z);
        }
    }*/
    fbm->GenUniformGrid2D(heights.data(), baseX, baseZ, 32, 32, 1.0f, 1.0f, seed);
    // Нормализация (FBM с 3 октавами дает диапазон примерно [-1.5, 1.5])
    for (int i = 0; i < 32 * 32; ++i) {
        heights[i] = 10.0f + heights[i] * 8.0f;
    }

    fnSimplex_->GenUniformGrid3D(caveNoise.data(), baseX, baseY, baseZ, 32, 32, 32, 1.0f, 1.0f, 1.0f, seed);

    for (int y = 0; y < 32; ++y) {
        int worldY = baseY + y;
        for (int z = 0; z < 32; ++z) {
            int z_offset = z * 32;
            for (int x = 0; x < 32; ++x) {
                float terrainHeight = heights[z_offset + x];

                uint16_t block = 0;
                if (worldY < terrainHeight) { [[likely]]
                    if (worldY < terrainHeight - 4) {
                        block = 1; // stone
                    } else if (worldY < terrainHeight - 1) {
                        block = 3; // dirt
                    } else if (worldY < terrainHeight) {
                        block = 2; // grass
                    }
                } else if (worldY < 0) { [[unlikely]] //or 64?
                    block = 4; // water
                }

                int idx = (z_offset + y) * 32 + x;

                // Пещеры (caveNoise — плоский массив float)
                if (block == 0 || block == 4) {
                    c.SetBlock(x, y, z, block);
                    continue;
                }

                if (caveNoise[idx] > 0.9f) {
                    block = 0;
                }

                c.SetBlock(x, y, z, block);
            }
        }
    }

    int32_t minTerrainHeight = std::numeric_limits<int32_t>::max();
    for (int idx = 0; idx < 32 * 32; ++idx) {
        if (heights[idx] < minTerrainHeight) {
            minTerrainHeight = static_cast<int32_t>(heights[idx]);
        }
    }

    // Lazy-init ore configuration from JSON
    static bool oreConfigLoaded = false;
    if (!oreConfigLoaded) {
        OreConfig::instance().load("data/registry/ores.json");
        oreConfigLoaded = true;
    }

    OreGenerator oreGen(seed);
    oreGen.generateOres(cx, cy, cz, c.getBlocks(), 32);
}
