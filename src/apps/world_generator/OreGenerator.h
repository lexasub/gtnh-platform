#pragma once
#include <cstdint>
#include <array>

class Chunk;
struct VeinDef;

class OreGenerator {
    // Размер региона в чанках (3x3 чанка = 96x96 блоков)
    static constexpr int REGION_SIZE_CHUNKS = 3;
public:
    static constexpr int REGION_SIZE_BLOCKS = REGION_SIZE_CHUNKS * 32;
    explicit OreGenerator(int32_t worldSeed);

    void generateOres(int32_t chunkX, int32_t chunkY, int32_t chunkZ,
                      std::array<uint16_t, 32*32*32>& blocks, int32_t chunkSize = 32);

private:
    int32_t m_worldSeed;


    uint32_t hashRegion(int32_t rx, int32_t rz, uint32_t seed) const;
    const VeinDef* selectVein(int32_t y) const;
};