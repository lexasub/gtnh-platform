#pragma once
#include <cstdint>
#include <optional>

namespace simcore {

struct TransformResult {
  uint16_t new_block_id;
  uint8_t new_meta;
};

// Returns transformed block if rule matches, otherwise std::nullopt
std::optional<TransformResult> applyBlockTransform(uint16_t expected_block_id,
                                                   uint16_t new_block_id,
                                                   uint8_t new_meta);

} // namespace simcore