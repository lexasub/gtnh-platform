#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

namespace simcore {

struct TransformResult {
  uint16_t new_block_id;
  uint8_t new_meta;
};

// Data-driven "placed X on Y → Z" rules, loaded from
// data/registry/transforms.csv (expected,new,result_id[,result_meta]).
class BlockTransforms {
public:
  static BlockTransforms* Load(const char* csv_path);

  static BlockTransforms* instance() { return instance_; }
  static void setInstance(BlockTransforms* transforms) { instance_ = transforms; }

  std::optional<TransformResult> Apply(uint16_t expected_block_id,
                                       uint16_t new_block_id) const;

private:
  std::unordered_map<uint64_t, TransformResult> rules_;
  static BlockTransforms* instance_;
};

} // namespace simcore
