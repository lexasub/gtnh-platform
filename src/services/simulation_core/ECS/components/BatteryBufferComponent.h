#pragma once
#include <cstdint>

namespace simcore {

struct BatteryBufferComponent {
  uint32_t capacity;  // EU storage (LV=40000, MV=150000, HV=600000)
  int32_t stored;     // Current EU
  uint8_t tier;       // 0=ULV...3=HV
  int32_t maxInput;   // Max EU/tick input (LV=32, MV=128, HV=512)
  int32_t chargeRate; // EU/tick per charging slot (LV=8, MV=32, HV=128)
  uint8_t numSlots;   // Charging slots (1 for LV, 2 for MV, 4 for HV)
};

} // namespace simcore