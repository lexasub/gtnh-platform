#pragma once
#include <cstdint>
#include <vector>

#include "Types.h"

struct ItemStack {
  uint16_t item_id = 0;
  uint8_t count = 0;
  uint16_t meta = 0;
};

struct InventoryState {
  std::vector<ItemStack> slots;
  BlockPos position;
  bool open = false;
  uint64_t player_id = 0;

  // Drag-and-drop state
  bool isDragging = false;
  ItemStack dragItem;
  int dragSourceSlot = -1;
  int dragHoverSlot = -1; // Slot under cursor while dragging

  // Hotbar selection
  int selectedSlot = -1;

  // Hovered item (updated each frame by render code)
  uint16_t hoveredItemId = 0;

  // Hovered slot index (set by SlotGridComponent each frame, -1 = none)
  int16_t hoveredSlot = -1;

  // Drag state (debug overlay)
  bool dragEnabled = true;
  bool dropEnabled = true;
  bool shiftDropEnabled = true;
};
