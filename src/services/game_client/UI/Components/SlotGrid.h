#pragma once

#include <cstdint>
#include <functional>
#include <vector>

struct ImDrawList;
struct ImVec2;

struct InventoryState;
class DragManager;
#include "Common/Inventory.h"

// ── Slot style configuration ────────────────────────────────────────────────
struct SlotStyle {
  int size = 40;
  int padding = 2;
  bool showNumbers = true;
  bool drawBackground = true;
  uint32_t backgroundColor = 0; // IM_COL32(128,128,128,40)
  uint32_t selectedColor = 0;   // IM_COL32(255,255,255,60)
  uint32_t selectedBorder = 0;  // IM_COL32(255,255,255,200)
  uint32_t borderColor = 0;     // IM_COL32(80,80,80,120) for cell borders
  int borderThickness = 1;
};

// ── Free rendering functions ───────────────────────────────────────────────

// Returns true when the slot is activated (mouse press) for drag pick-up.
bool RenderSlot(const ItemStack &stack, bool selected, ImDrawList *dl,
                const SlotStyle &style = {});

// Render a rect-grid of slots from a vector.
// startIndex + count define which elements of the vector to render.
// Returns the GLOBAL index of the clicked slot, or -1.
int RenderSlotGrid(const std::vector<ItemStack> &slots, int startIndex,
                   int count, int cols, int selectedSlot = -1,
                   const SlotStyle &style = {},
                   std::function<void(int, int, bool)> *clickCb = nullptr);

// Render a horizontal hotbar at bottom-center of the screen.
void RenderHotbar(const std::vector<ItemStack> &slots, int selectedSlot,
                  const SlotStyle &style = {});

// ── Stateful component ─────────────────────────────────────────────────────
// Keeps hover state across frames for drag-and-drop interactions.
class SlotGridComponent {
public:
  explicit SlotGridComponent(std::vector<ItemStack> &slots);

  // ── Layout ──────────────────────────────────────────────────────────────
  void SetStyle(const SlotStyle &style) { style_ = style; }
  void SetRange(int startIndex, int count, int cols);
  void SetSelectedSlot(int slot) { selectedSlot_ = slot; }
  int GetSelectedSlot() const { return selectedSlot_; }

  // ── Inventory integration ──────────────────────────────────────────────
  void SetInventory(InventoryState &inv);

  // ── Drag & drop ─────────────────────────────────────────────────────────
  void SetDragManager(DragManager *dm) { dm_ = dm; }

  // ── Click callback ──────────────────────────────────────────────────────
  using ClickCallback = std::function<bool(int slot, int button, bool shift)>;
  void SetClickCallback(ClickCallback cb) { clickCb_ = std::move(cb); }

  // ── Rendering ───────────────────────────────────────────────────────────
  int Render();

  // ── Query ──────────────────────────────────────────────────────────────
  int GetHoveredSlot() const { return hoveredSlot_; }

private:
  std::vector<ItemStack> &slots_;
  InventoryState *inv_ = nullptr;
  SlotStyle style_;
  int startIndex_ = 0;
  int count_ = 0;
  int cols_ = 10;
  int selectedSlot_ = -1;
  int hoveredSlot_ = -1;
  ClickCallback clickCb_;
  DragManager *dm_ = nullptr;
};
