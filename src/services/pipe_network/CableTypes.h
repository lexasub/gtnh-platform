#pragma once
#include <common/ItemId.h>
#include <cstdint>
#include <unordered_map>

namespace gtnh {
namespace pipe_network {

struct CableDef {
  uint16_t block_id;
  uint8_t tier;
  const char *name;
  float loss_per_block;
  uint32_t max_voltage;
  uint32_t ampacity;
};

// IDs must match data/registry/items.csv (cable_tin=1111:01:0 .. cable_platinum=1111:01:5)
// and ItemId::isCable()'s packed range [1111:01:0, 1111:10:0).
const std::unordered_map<uint16_t, CableDef> CABLE_DEFS = {
    {ItemId::pack("1111:01:0"), {ItemId::pack("1111:01:0"), 1, "cable_tin", 0x05f5e100, 0x20, 0x20}},
    {ItemId::pack("1111:01:1"), {ItemId::pack("1111:01:1"), 1, "cable_copper", 0x03f5e100, 0x20, 0x40}},
    {ItemId::pack("1111:01:2"), {ItemId::pack("1111:01:2"), 2, "cable_gold", 0x02f5e100, 0x80, 0x80}},
    {ItemId::pack("1111:01:3"), {ItemId::pack("1111:01:3"), 2, "cable_aluminium", 0x02f5e100, 0x80, 0x100}},
    {ItemId::pack("1111:01:4"), {ItemId::pack("1111:01:4"), 3, "cable_tungsten", 0x01f5e100, 0x200, 0x200}},
    {ItemId::pack("1111:01:5"), {ItemId::pack("1111:01:5"), 4, "cable_platinum", 0x00f5e100, 0x800, 0x400}},
};

inline bool isCableBlock(uint16_t block_id) {
  return CABLE_DEFS.count(block_id) > 0;
}

inline const CableDef *getCableDef(uint16_t block_id) {
  auto it = CABLE_DEFS.find(block_id);
  return it != CABLE_DEFS.end() ? &it->second : nullptr;
}

} // namespace pipe_network
} // namespace gtnh