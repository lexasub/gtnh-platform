#pragma once

#include <cstdint>
#include <string_view>

#include "Common/Types.h"

// ── ISidePanel — side panel interface ────────────────────────────
// Side panels are NOT windows; they are lightweight content panels
// that render adjacently to machine windows (NEI-style).
// UIManager owns and renders them, but they do not go through
// OpenExclusive/CloseAll.
class ISidePanel {
public:
  virtual ~ISidePanel() = default;

  // Unique panel name (for debug / identification).
  virtual std::string_view Name() const = 0;

  // Render the panel content. Called once per frame from UIManager.
  // playerInv is the shared player inventory.
  virtual void Render(class InventoryState *playerInv) = 0;

  // Panel visibility (toggled by hotkey or context).
  virtual bool IsVisible() const = 0;
  virtual void SetVisible(bool visible) = 0;

  // Mouse capture hint — if panel wants to block clicks, return true.
  virtual bool WantsMouseCapture() const { return false; }

  // Key event dispatch (for hotkeys while panel is open).
  virtual bool OnKeyEvent([[maybe_unused]] int key, [[maybe_unused]] int action,
                          [[maybe_unused]] int mods) {
    return false;
  }

  // Returns the block position this panel is associated with.
  // Override in panels that are position-aware (e.g., NeiPanel for a specific
  // machine). Default returns empty BlockPos — panel is not tied to any block.
  virtual BlockPos GetTargetBlock() const { return BlockPos{}; }
};
