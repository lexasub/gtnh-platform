#include "OreGenerator.h"
#include <cmath>

OreGenerator::OreGenerator(int32_t worldSeed) : m_worldSeed(worldSeed) {}

void OreGenerator::generateOres(int32_t chunkX, int32_t chunkZ,
                                int32_t baseHeight,
                                std::array<uint16_t, 32*32*32>& blocks,
                                int32_t chunkSize) {
    auto& config = OreConfig::instance();
    int32_t baseX = chunkX * chunkSize;
    int32_t baseZ = chunkZ * chunkSize;
    
    for (size_t oreIdx = 0; oreIdx < config.allOres().size(); ++oreIdx) {
        const auto& ore = config.allOres()[oreIdx];
        
        for (int32_t x = 0; x < chunkSize; x++) {
            for (int32_t z = 0; z < chunkSize; z++) {
                int32_t worldX = baseX + x;
                int32_t worldZ = baseZ + z;
                
                // Only generate below terrain surface
                int32_t terrainHeight = baseHeight;
                
                for (int32_t y = ore.min_y; y <= std::min(ore.max_y, terrainHeight - 1); y++) {
                    if (y < 0) continue;
                    
                    int idx = (y * chunkSize + z) * chunkSize + x;
                    if (blocks[idx] != 1) continue;  // only replace stone
                    
                    float fx = worldX * ore.frequency;
                    //float fy = worldY * ore.frequency;
                    float fz = worldZ * ore.frequency;
                    float noise = std::sin(fx) /* std::sin(fy)*/ * std::sin(fz);
                    
                    if (noise > ore.threshold) {
                        blocks[idx] = ore.block_id;
                    }
                }
            }
        }
    }
}

float OreGenerator::calculateOreNoise(const OreDef& ore,
                                       int32_t worldX, int32_t worldY, int32_t worldZ) const {
    float fx = worldX * ore.frequency;
    float fy = worldY * ore.frequency;
    float fz = worldZ * ore.frequency;
    return std::sin(fx) * std::sin(fy) * std::sin(fz);
}

bool OreGenerator::isInHeightRange(const OreDef& ore, int32_t y) const {
    return y >= ore.min_y && y <= ore.max_y;
}

bool OreGenerator::shouldGenerateInChunk(int32_t chunkX, int32_t chunkZ, const OreDef& ore) const {
    // Simplified - no rarity filtering for config-driven generation
    (void)chunkX; (void)chunkZ; (void)ore;
    return true;
}

float OreGenerator::calculateDensityNoise(const OreDef& ore,
                                          int32_t worldX, int32_t worldY, int32_t worldZ) const {
    // Simplified - no density noise for config-driven generation
    (void)ore; (void)worldX; (void)worldY; (void)worldZ;
    return 0.0f;
}