#pragma once
#include <cstdint>
#include <unordered_map>

namespace gtnh {
namespace pipe_network {

struct CableDef {
    uint16_t block_id;
    uint8_t tier;
    const char* name;
    float loss_per_block;
    uint32_t max_voltage;
    uint32_t ampacity;
};

// IDs must match data/registry/items.csv (cable_tin=66..cable_platinum=71)
const std::unordered_map<uint16_t, CableDef> CABLE_DEFS = {
    {66, {66, 1, "cable_tin",      0x05f5e100, 0x20,    0x20}},
    {67, {67, 1, "cable_copper",   0x03f5e100, 0x20,    0x40}},
    {68, {68, 2, "cable_gold",     0x02f5e100, 0x80,    0x80}},
    {69, {69, 2, "cable_alu",      0x02f5e100, 0x80,    0x100}},
    {70, {70, 3, "cable_tungsten", 0x01f5e100, 0x200,   0x200}},
    {71, {71, 4, "cable_platinum", 0x00f5e100, 0x800,   0x400}},
};

inline bool isCableBlock(uint16_t block_id) {
    return CABLE_DEFS.count(block_id) > 0;
}

inline const CableDef* getCableDef(uint16_t block_id) {
    auto it = CABLE_DEFS.find(block_id);
    return it != CABLE_DEFS.end() ? &it->second : nullptr;
}

} // namespace pipe_network
} // namespace gtnh