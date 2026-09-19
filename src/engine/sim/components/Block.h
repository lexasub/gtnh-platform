#pragma once
#include <cstdint>

namespace simcore {

struct Block {
  uint16_t id = 0;
  uint8_t meta = 0;
  uint32_t mb_id = 0;

  Block() = default;
  Block(uint16_t id_, uint8_t meta_, uint32_t mb_id_)
      : id(id_), meta(meta_), mb_id(mb_id_) {}
};

} // namespace simcore