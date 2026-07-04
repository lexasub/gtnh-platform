#pragma once

#include "Common/Inventory.h"
#include "common/SlotContainer.h"
#include <array>
#include <cstdint>

class DragManager;

class CraftingGrid {
  static constexpr int kGridFlag = 100;
  SlotContainer<9, ItemStack> slots_{};
  ItemStack result_{};

public:
  CraftingGrid() {
    slots_.setOnChange([this](int, ItemStack, ItemStack) { Recalc(); });
  }

  const SlotContainer<9, ItemStack> &Slots() const { return slots_; }
  const ItemStack &GetResult() const { return result_; }

  // Override result (used by OnCraftResponse to show crafted item)
  void SetResult(const ItemStack &result) { result_ = result; }

  // Set all 9 grid slots from server response (consumed grid after crafting)
  void SetSlots(const std::array<ItemStack, 9> &slots);

  // Handle slot activation (click) using DragManager for drag state
  void HandleActivate(int gridIdx, InventoryState &inv, DragManager &dm);

  // Handle cancel drag using DragManager
  void HandleCancel(DragManager &dm);

  // Clear all slots + result
  void Clear();

  // Recalculate result via Crafting::MatchGrid
  void Recalc();
};
