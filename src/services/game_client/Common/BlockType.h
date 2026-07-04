#pragma once

#include <cstdint>
#include <string_view>

enum class BlockType : uint16_t {
  Unknown = 0,
  CraftingTable = 14,
  Chest = 37,
};

// Number of known BlockType values (excluding Unknown).
inline constexpr size_t kBlockTypeCount = 2;

// Convert BlockType ↔ uint16_t (for network/FlatBuffers boundary)
constexpr uint16_t ToUint16(BlockType t) noexcept {
  return static_cast<uint16_t>(t);
}
constexpr BlockType ToBlockType(uint16_t v) noexcept {
  // Enum values now match items.csv IDs directly
  if (v == static_cast<uint16_t>(BlockType::CraftingTable) ||
      v == static_cast<uint16_t>(BlockType::Chest))
    return static_cast<BlockType>(v);
  return BlockType::Unknown;
}

constexpr std::string_view ToString(BlockType t) noexcept {
  switch (t) {
  case BlockType::CraftingTable:
    return "Crafting Table";
  case BlockType::Chest:
    return "Chest";
  default:
    return "Unknown";
  }
}
// Machines are runtime-only via MachineRegistry — no compile-time enum entries
