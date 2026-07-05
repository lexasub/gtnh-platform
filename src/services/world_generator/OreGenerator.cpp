#include "OreGenerator.h"
#include "OreConfig.h"
#include "FastNoise/FastNoise.h"
#include <cmath>
#include <random>
#include <algorithm>

// Thread-local нод FastNoise (SIMD-оптимизирован внутри библиотеки)
namespace {
    thread_local std::mt19937 rng;
    thread_local std::uniform_int_distribution<int32_t> posDist(0, OreGenerator::REGION_SIZE_BLOCKS - 1);
    thread_local std::uniform_int_distribution<int> emptyChance(0, 99);
    thread_local std::uniform_int_distribution<int32_t> yDist(5, 60);
    thread_local std::uniform_real_distribution<float> sporadicChance(0.0f, 1.0f);
    thread_local FastNoise::SmartNode<FastNoise::Simplex> fnOre3D_ = FastNoise::New<FastNoise::Simplex>();
    thread_local std::vector<const VeinDef*> candidates;
    // Буфер для 3D шума (переиспользуется, чтобы не аллоцировать память)
    thread_local std::array<float, 32*32*32> noiseBuffer;
}

OreGenerator::OreGenerator(int32_t worldSeed) : m_worldSeed(worldSeed) {}

// Детерминированный хеш для региона
uint32_t OreGenerator::hashRegion(int32_t rx, int32_t rz, uint32_t seed) const {
    uint32_t h = seed ^ (rx * 374761393u) ^ (rz * 668265263u);
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

const VeinDef* OreGenerator::selectVein(int32_t y) const {
    candidates.clear();
    auto& config = OreConfig::instance();
    int32_t totalWeight = 0;

    for (const auto& vein : config.allVeins()) {
        if (y >= vein.min_y && y <= vein.max_y) {
            candidates.push_back(&vein);
            totalWeight += vein.weight;
        }
    }

    if (candidates.empty() || totalWeight == 0) return nullptr;

    std::uniform_int_distribution<int32_t> dist(0, totalWeight - 1); //TODO May be add cache for popular totalWeight?
    int32_t roll = dist(rng);

    int32_t currentWeight = 0;
    for (const auto* v : candidates) {
        currentWeight += v->weight;
        if (roll < currentWeight) return v;
    }
    return candidates.back();
}

void OreGenerator::generateOres(int32_t chunkX, int32_t chunkY, int32_t chunkZ,
                                std::array<uint16_t, 32*32*32>& blocks, int32_t chunkSize) {
    auto& config = OreConfig::instance();
    if (config.allVeins().empty()) return;

    int32_t baseX = chunkX * chunkSize;
    int32_t baseY = chunkY * chunkSize;
    int32_t baseZ = chunkZ * chunkSize;

    // В GTNH жила может пересекать границы чанков.
    // Поэтому мы проверяем не только свой регион, но и 8 соседних (3x3 grid).
    int32_t myRx = chunkX >= 0 ? chunkX / REGION_SIZE_CHUNKS : (chunkX - REGION_SIZE_CHUNKS + 1) / REGION_SIZE_CHUNKS;
    int32_t myRz = chunkZ >= 0 ? chunkZ / REGION_SIZE_CHUNKS : (chunkZ - REGION_SIZE_CHUNKS + 1) / REGION_SIZE_CHUNKS;

    for (int32_t rx = myRx - 1; rx <= myRx + 1; ++rx) {
        for (int32_t rz = myRz - 1; rz <= myRz + 1; ++rz) {

            // 1. Хеш региона
            uint32_t regionSeed = hashRegion(rx, rz, m_worldSeed + config.seedOffset());
            rng.seed(regionSeed);

            // 2. Шанс, что в регионе вообще есть жила (80%)
            if (emptyChance(rng) < 20) continue;

            // 3. Глубина центра жилы
            int32_t centerY = yDist(rng);

            // 4. Выбор типа жилы
            const VeinDef* vein = selectVein(centerY);
            if (!vein) continue;

            // 5. Координаты центра жилы внутри региона
            int32_t regionBaseX = rx * REGION_SIZE_BLOCKS;
            int32_t regionBaseZ = rz * REGION_SIZE_BLOCKS;

            float centerX = regionBaseX + posDist(rng) + 0.5f;
            float centerZ = regionBaseZ + posDist(rng) + 0.5f;

            // 6. Радиус жилы (зависит от density)
            float radius = 12.0f + (vein->density * 10.0f);
            float radiusSq = radius * radius;
            float coreRadiusSq = (radius * 0.4f) * (radius * 0.4f);
            float secondaryRadiusSq = (radius * 0.7f) * (radius * 0.7f);

            // ==========================================
            // SIMD ОПТИМИЗАЦИЯ 1: AABB CULLING
            // Проверяем, пересекается ли сфера жилы с текущим чанком.
            // Если нет — пропускаем, не тратим CPU на шум и расстояния.
            // ==========================================
            float closestX = std::max((float)baseX, std::min(centerX, (float)(baseX + chunkSize)));
            float closestZ = std::max((float)baseZ, std::min(centerZ, (float)(baseZ + chunkSize)));
            float closestY = std::max((float)baseY, std::min((float)centerY, (float)(baseY + chunkSize)));

            float dx = closestX - centerX;
            float dy = closestY - centerY;
            float dz = closestZ - centerZ;

            if ((dx * dx + dy * dy + dz * dz) > radiusSq) {
                continue; // Жила слишком далеко, пропускаем регион
            }

            // После AABB culling, находим пересечение жилы с чанком
            int32_t startX = std::max(0, (int32_t)(centerX - radius) - baseX);
            int32_t endX = std::min(chunkSize - 1, (int32_t)(centerX + radius) - baseX);
            int32_t startY = std::max(0, (int32_t)(centerY - radius) - baseY);
            int32_t endY = std::min(chunkSize - 1, (int32_t)(centerY + radius) - baseY);
            int32_t startZ = std::max(0, (int32_t)(centerZ - radius) - baseZ);
            int32_t endZ = std::min(chunkSize - 1, (int32_t)(centerZ + radius) - baseZ);

            int32_t width = endX - startX + 1;
            int32_t height = endY - startY + 1;
            int32_t depth = endZ - startZ + 1;

            // ==========================================
            // SIMD ОПТИМИЗАЦИЯ 2: FASTNOISE 3D
            // Генерируем шум сразу для всего чанка.
            // Внутри FastNoise использует AVX2/SSE инструкции.
            // ==========================================
            // Генерируем шум только для этой области
            fnOre3D_->GenUniformGrid3D(noiseBuffer.data(), baseX + startX, baseY + startY, baseZ + startZ, width, height, depth, 0.1f, 0.1f, 0.1f, regionSeed);

            // ==========================================
            // SIMD ОПТИМИЗАЦИЯ 3: ВЕКТОРИЗУЕМЫЙ ЦИКЛ
            // Простая математика расстояний. Компилятор (GCC/Clang с -O3)
            // сам развернет это в SIMD-регистры.
            // ==========================================

            for (int32_t y = startY; y < endY; ++y) {
                int32_t worldY = baseY + y;
                float dy = worldY - centerY;
                float dySq = dy * dy;

                for (int32_t z = startZ; z < endZ; ++z) {
                    int32_t worldZ = baseZ + z;
                    float dz = worldZ - centerZ;
                    float dyzSq = dySq + dz * dz;

                    for (int32_t x = startX; x < endX; ++x) {
                        int32_t noiseIdx = ((y - startY) * depth + (z - startZ)) * width + (x - startX);
                        int32_t worldX = baseX + x;
                        auto &currentBlock = blocks[(y * chunkSize + z) * chunkSize + x];

                        // Заменяем ТОЛЬКО камень (ID = 1)
                        if (currentBlock != 1) continue;

                        float dx = worldX - centerX;
                        float distSq = dyzSq + dx * dx;

                        if (distSq < coreRadiusSq) {
                            currentBlock = vein->primary_id;
                        }
                        else if (distSq < secondaryRadiusSq) {
                            currentBlock = vein->secondary_id;
                        }
                        else if (distSq < radiusSq) {
                            // Sporadic (вкрапления) зависят от 3D шума
                            if (noiseBuffer[noiseIdx] > 0.5f) {
                                currentBlock = vein->sporadic_id;
                            }
                        }
                    }
                }
            }
        }
    }
}