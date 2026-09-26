#pragma once
#include <cstdint>
#include <vector>

#include <common/GameModePerm.h>

#include "Types.h"

// GameMode, GameModePerm (the permission matrix), IsDefinedGameMode and
// TryGameModeFromWire now live in src/common/GameModePerm.h, re-exported
// here unchanged (gp-t71g).
//
// They were extracted because the server has to enforce the same matrix to be
// authoritative, and simcore cannot include a header from the client tree
// without a layering inversion — which is the reason the placement path had no
// gate at all. This is the same move as engine/registry/coords/Coords.h, which
// its own header documents as "extracted from game_client/Common/Types.h".
//
// Including this header keeps resolving every one of those names by the same
// unqualified name as before, so no call site anywhere in the client, the UI
// or the three gameclient test binaries had to change.

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
