#pragma once
#include <cstdint>
#include "../PatternLibrary.h"  // for HatchType

namespace simcore {

struct HatchSlot {
  HatchType type = HatchType::NONE;
  uint32_t world_x = 0;
  uint32_t world_y = 0;
  uint32_t world_z = 0;
  uint16_t slot_start = 0;  // start index in InventoryContainer
  uint16_t slot_end = 0;    // exclusive end index
  uint8_t side_config = 5;  // default ANY

  // Slot count by hatch type (hatch types with no inventory use 0)
  static constexpr uint16_t kSlotsPerHatch(HatchType t) {
    if (t == HatchType::ITEM_IN || t == HatchType::ITEM_OUT) return 4;
    return 0;
  }

  bool hasItemSlots() const { return kSlotsPerHatch(type) > 0; }
};

} // namespace simcore
