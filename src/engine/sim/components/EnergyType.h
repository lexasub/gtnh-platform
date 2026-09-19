#pragma once
#include <cstdint>

namespace simcore {

enum class EnergyType : uint8_t {
  ELECTRICITY = 0,
  HEAT = 1,
  STEAM = 2,
  ROTATION = 3,
};

} // namespace simcore
