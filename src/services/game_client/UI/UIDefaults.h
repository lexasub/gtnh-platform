#pragma once

#include "Common/BlockType.h"
#include "Common/Types.h"

class UIManager;
struct InventoryState;

namespace UIDefaults {

// Creates and registers player UI windows (inventory, creative menu).
// Call once from GameClient::Init.
void RegisterPlayerUI(UIManager& mgr, InventoryState& invState);

// Opens UI window for a world block (workbench, furnace, etc.).
// Returns true if a window was opened.
// Call from GameClient::Update on right-click.
bool TryOpenBlockUI(UIManager& mgr, uint16_t blockId, const BlockPos& pos);

}  // namespace UIDefaults
