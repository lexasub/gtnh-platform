#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct OreDef {
    std::string name;
    uint16_t block_id;
    int32_t min_y;
    int32_t max_y;
    float threshold;
    float frequency;
    uint8_t vein_size;
    float density;
    float rarity;
};

class OreConfig {
public:
    static OreConfig& instance();
    bool load(const std::string& path);
    const std::vector<OreDef>& allOres() const;
    const OreDef* getOre(uint16_t block_id) const;
    int32_t seedOffset() const;
private:
    std::vector<OreDef> m_ores;
    int32_t m_seedOffset = 12345;
};