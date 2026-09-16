#pragma once

#include <cstdint>

namespace simcore {

// Steam is kept separate from the turbine's EU output buffer. PipeNetwork
// delivers steam through the FluidClient; the turbine converts it to EU on
// the SimulationCore tick.
struct SteamTurbineComponent {
  std::uint16_t steam_item_id = 0;
  std::int32_t steam_stored = 0;
  std::int32_t steam_capacity = 1000;
  std::int32_t steam_max_input = 32;
  std::int32_t eu_per_steam = 1;
  std::int32_t eu_stored = 0;
  std::int32_t eu_capacity = 10000;
  std::int32_t eu_max_output = 32;
  bool request_pending = false;
};

} // namespace simcore
