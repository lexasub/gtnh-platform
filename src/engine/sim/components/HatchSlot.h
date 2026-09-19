#pragma once
#include <cstdint>
#include <engine/sim/PatternLibrary.h>

namespace simcore {

struct HatchSlot {
  HatchType type = HatchType::NONE;
  uint32_t world_x = 0;
  uint32_t world_y = 0;
  uint32_t world_z = 0;
  uint16_t slot_start = 0;
  uint16_t slot_end = 0;
  uint8_t side_config = 5;
  uint8_t tier = 0;
  bool present = false;

  static constexpr uint16_t kSlotsPerHatch(HatchType t) {
    if (t == HatchType::ITEM_IN || t == HatchType::ITEM_OUT) return 4;
    return 0;
  }

  bool hasItemSlots() const { return kSlotsPerHatch(type) > 0; }
};

} // namespace simcore
