#pragma once
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace simcore {

struct DropInfo {
  uint16_t result_id;
  uint8_t count;
  uint8_t meta;
};

// Maps a broken block to its drop: "breaking stone yields cobblestone".
// Rules are loaded from data/registry/drops.csv (source,result[,count[,meta]]).
// Callers fall back to the block itself when Get() returns nullptr.
class BlockDrops {
public:
  static BlockDrops* Load(const char* csv_path);

  static BlockDrops* instance() { return instance_; }
  static void setInstance(BlockDrops* drops) { instance_ = drops; }

  const DropInfo* Get(uint16_t block_id) const;

private:
  std::unordered_map<uint16_t, DropInfo> drops_;
  static BlockDrops* instance_;
};

} // namespace simcore
