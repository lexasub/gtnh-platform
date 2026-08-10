#pragma once
#include <cstdint>
#include <vector>

#include "Types.h"

struct ItemStack {
  uint16_t item_id = 0;
  uint8_t count = 0;
  uint16_t meta = 0;
};

// ──────────────────────────────────────────────────────────────────────────
// GameMode — client-side game mode (server trusts client for now)
// ──────────────────────────────────────────────────────────────────────────
enum class GameMode : uint8_t {
  SURVIVAL = 0,
  CREATIVE = 1,
  ADVENTURE = 2,
  SPECTATOR = 3,
};

inline const char* GameModeName(GameMode mode) {
  switch (mode) {
    case GameMode::SURVIVAL:  return "SURVIVAL";
    case GameMode::CREATIVE:  return "CREATIVE";
    case GameMode::ADVENTURE: return "ADVENTURE";
    case GameMode::SPECTATOR: return "SPECTATOR";
  }
  return "UNKNOWN";
}

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

  // Server-owned cursor stack (authoritative click model). Rendered as the
  // drag preview; replaced wholesale from each InventoryUpdate snapshot.
  ItemStack cursor;

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

  // Game mode (client-authoritative for now, server trusts client)
  GameMode gameMode = GameMode::CREATIVE;
};
