#include "WorldGenerator.h"

#include <array>
#include <cstdint>
#include <cmath>
#include <limits>
#include <mutex>

#include "FastNoise/FastNoise.h"
#include "OreConfig.h"
#include "OreGenerator.h"
#include "common/ItemId.h"

namespace {
    constexpr int CHUNK_SIZE     = 32;
    constexpr int CHUNK_SIZE_SQ  = CHUNK_SIZE * CHUNK_SIZE;
    constexpr int CHUNK_SIZE_CUB = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;
    constexpr int SEED           = 100;

    constexpr uint16_t BLOCK_AIR   = ItemId::pack("0:0:0");
    constexpr uint16_t BLOCK_STONE = ItemId::pack("0:0:1");
    constexpr uint16_t BLOCK_GRASS = ItemId::pack("0:0:8");
    constexpr uint16_t BLOCK_DIRT  = ItemId::pack("0:0:7");
    constexpr uint16_t BLOCK_WATER = ItemId::pack("1111:11:0");

    // Параметры шума
    constexpr float BASE_FREQ = 0.02f;   // 1/50
    constexpr float CONT_FREQ = 0.005f;  // 1/200
    constexpr float CAVE_FREQ = 0.03f;   // 1/33.33

    constexpr float BASE_AMP    = 12.0f;
    constexpr float CONT_AMP    = 20.0f;
    constexpr float BASE_HEIGHT = 64.0f;

    // === Шумы ===
    thread_local FastNoise::SmartNode<FastNoise::Perlin> basePerlin = FastNoise::New<FastNoise::Perlin>();
    thread_local FastNoise::SmartNode<FastNoise::FractalFBm> baseFBM = FastNoise::New<FastNoise::FractalFBm>();

    thread_local FastNoise::SmartNode<FastNoise::Simplex> contSimplex = FastNoise::New<FastNoise::Simplex>();
    thread_local FastNoise::SmartNode<FastNoise::FractalFBm> contFBM = FastNoise::New<FastNoise::FractalFBm>();

    thread_local FastNoise::SmartNode<FastNoise::Simplex> caveSimplex = FastNoise::New<FastNoise::Simplex>();
    thread_local FastNoise::SmartNode<FastNoise::FractalFBm> caveFBM = FastNoise::New<FastNoise::FractalFBm>();

    // === Буферы ===
    thread_local std::array<float, CHUNK_SIZE_SQ>  baseNoise;
    thread_local std::array<float, CHUNK_SIZE_SQ>  contNoise;
    thread_local std::array<float, CHUNK_SIZE_SQ>  heights;
    thread_local std::array<float, CHUNK_SIZE_CUB> caveNoise;

    OreGenerator oreGen(SEED);

    thread_local bool initialized = false;

    void init() {
        if (initialized) return;

        basePerlin->SetScale(1.0f / BASE_FREQ);
        baseFBM->SetSource(basePerlin);
        baseFBM->SetOctaveCount(3);
        baseFBM->SetLacunarity(2.0f);
        baseFBM->SetGain(0.5f);

        contSimplex->SetScale(1.0f / CONT_FREQ);
        contFBM->SetSource(contSimplex);
        contFBM->SetOctaveCount(2);
        contFBM->SetLacunarity(2.0f);
        contFBM->SetGain(0.5f);

        caveSimplex->SetScale(1.0f / CAVE_FREQ);
        caveFBM->SetSource(caveSimplex);
        caveFBM->SetOctaveCount(2);
        caveFBM->SetLacunarity(3.0f);
        caveFBM->SetGain(0.6f);

        initialized = true;
    }

    inline int idx2(int x, int z) { return z * CHUNK_SIZE + x; }
    inline int idx3(int x, int y, int z) { return (z * CHUNK_SIZE + y) * CHUNK_SIZE + x; }
}


void WorldGenerator::GenerateTerrain(Chunk& c, int cx, int cy, int cz) {
    const int baseX = cx * CHUNK_SIZE;
    const int baseZ = cz * CHUNK_SIZE;
    const int baseY = cy * CHUNK_SIZE;
    init();

    // === 2D шум: высоты ===
    baseFBM->GenUniformGrid2D(baseNoise.data(), baseX, baseZ, CHUNK_SIZE, CHUNK_SIZE, 1.0f, 1.0f, SEED);
    contFBM->GenUniformGrid2D(contNoise.data(), baseX, baseZ, CHUNK_SIZE, CHUNK_SIZE, 1.0f, 1.0f, SEED + 1);

    for (int i = 0; i < CHUNK_SIZE_SQ; ++i) {
        heights[i] = BASE_HEIGHT + baseNoise[i] * BASE_AMP + contNoise[i] * CONT_AMP;
    }

    // === 3D шум: пещеры ===
    caveFBM->GenUniformGrid3D(caveNoise.data(), baseX, baseY, baseZ,
                              CHUNK_SIZE, CHUNK_SIZE, CHUNK_SIZE,
                              1.0f, 1.0f, 1.0f, SEED + 2);


    for (int y = 0; y < CHUNK_SIZE; ++y) {
        int worldY = baseY + y;
        for (int z = 0; z < CHUNK_SIZE; ++z) {
            for (int x = 0; x < CHUNK_SIZE; ++x) {
                float terrainHeight = heights[idx2(x, z)];

                uint16_t block = BLOCK_AIR;
                if (worldY < terrainHeight) { [[likely]]
                    if (worldY < terrainHeight - 4.0f)      block = BLOCK_STONE;
                    else if (worldY < terrainHeight - 1.0f) block = BLOCK_DIRT;
                    else                                    block = BLOCK_GRASS;
                } else if (worldY < 0) { [[unlikely]]
                    block = BLOCK_WATER;
                }

                if (block == BLOCK_AIR || block == BLOCK_WATER) {
                    c.SetBlock(x, y, z, block);
                    continue;
                }

                if (std::abs(caveNoise[idx3(x, y, z)]) < 0.12f && worldY < terrainHeight - 5.0f)
                    block = (worldY < 0) ? BLOCK_WATER : BLOCK_AIR;

                c.SetBlock(x, y, z, block);
            }
        }
    }

    static std::once_flag oreConfigFlag;
    std::call_once(oreConfigFlag, []{
        OreConfig::instance().load("data/registry/ores.json");
    });

    oreGen.generateOres(cx, cy, cz, c.getBlocks(), CHUNK_SIZE);
}