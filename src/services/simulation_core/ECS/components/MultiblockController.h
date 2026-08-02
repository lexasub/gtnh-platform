#pragma once
#include <cstdint>
#include <vector>

namespace simcore {

struct MultiblockController {
  uint64_t id = 0;
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t z = 0;
  uint32_t pattern_id = 0;
  std::vector<uint32_t> blocks; // packed positions (xyz)

  MultiblockController() = default;
  MultiblockController(uint64_t id_, uint32_t x_, uint32_t y_, uint32_t z_,
                       uint32_t pattern_id_, const std::vector<uint32_t> &blocks_)
      : id(id_), x(x_), y(y_), z(z_), pattern_id(pattern_id_), blocks(blocks_) {}
};

} // namespace simcore