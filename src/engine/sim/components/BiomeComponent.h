#pragma once
#include <cstdint>

namespace simcore {

struct BiomeComponent {
  uint16_t biome_id;

  BiomeComponent() = default;
  explicit BiomeComponent(uint16_t id) : biome_id(id) {}
};

} // namespace simcore
