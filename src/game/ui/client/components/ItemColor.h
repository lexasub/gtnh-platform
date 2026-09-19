#pragma once

#include <cstdint>
#include <imgui.h>

// Returns a deterministic color based on item_id for rendering item icons.
inline ImU32 ItemColor(uint16_t item_id) {
  return IM_COL32(item_id * 50 % 256, item_id * 80 % 256, item_id * 30 % 256,
                  255);
}
