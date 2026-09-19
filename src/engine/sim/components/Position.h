#pragma once
#include <cstdint>

namespace simcore {

struct Position {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t z = 0;

  Position() = default;
  Position(uint32_t x_, uint32_t y_, uint32_t z_) : x(x_), y(y_), z(z_) {}
};

} // namespace simcore