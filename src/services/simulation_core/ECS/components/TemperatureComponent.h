#pragma once
#include <cstdint>

namespace simcore {

struct TemperatureComponent {
  float temperature;
  float max_temperature;
  float heat_capacity;

  TemperatureComponent() = default;
  TemperatureComponent(float temp, float max_temp, float heat_cap)
      : temperature(temp), max_temperature(max_temp), heat_capacity(heat_cap) {}
};

} // namespace simcore
