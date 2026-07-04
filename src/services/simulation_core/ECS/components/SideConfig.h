#pragma once
#include <cstdint>

namespace simcore {

enum class SideRole : uint8_t {
  INPUT = 0,
  OUTPUT = 1,
  ENERGY = 2,
  FLUID_IN = 3,
  FLUID_OUT = 4,
  ANY = 5,
  NONE = 6
};

// GTNH face convention: DOWN=0, UP=1, NORTH=2, SOUTH=3, WEST=4, EAST=5
constexpr uint8_t DEFAULT_SIDE_CONFIG[6] = {5, 5, 5, 5, 5, 5}; // all ANY

inline constexpr const char *sideRoleName(SideRole role) {
  switch (role) {
  case SideRole::INPUT:
    return "INPUT";
  case SideRole::OUTPUT:
    return "OUTPUT";
  case SideRole::ENERGY:
    return "ENERGY";
  case SideRole::FLUID_IN:
    return "FLUID_IN";
  case SideRole::FLUID_OUT:
    return "FLUID_OUT";
  case SideRole::ANY:
    return "ANY";
  case SideRole::NONE:
    return "NONE";
  default:
    return "UNKNOWN";
  }
}

// Cycle to next role (GTNH wrench interaction style)
inline constexpr uint8_t nextSideRole(uint8_t current, bool hasFluid,
                                      bool hasEnergy) {
  switch (current) {
  case 0:
    return hasEnergy
               ? 2
               : (hasFluid ? 3 : 1); // INPUT -> ENERGY (or FLUID_IN, or OUTPUT)
  case 1:
    return 0; // OUTPUT -> INPUT
  case 2:
    return 5; // ENERGY -> ANY
  case 3:
    return 4; // FLUID_IN -> FLUID_OUT
  case 4:
    return (hasFluid ? 3 : 0); // FLUID_OUT -> FLUID_IN (or INPUT)
  case 5:
    return 0; // ANY -> INPUT
  default:
    return 5; // NONE -> ANY
  }
}

} // namespace simcore