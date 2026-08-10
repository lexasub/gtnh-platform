#pragma once

#include "Common/Inventory.h"
#include "common/SlotContainer.h"
#include <array>
#include <cstdint>
#include <functional>

class CraftingGrid {
  SlotContainer<9, ItemStack> slots_{};
  ItemStack result_{};
  uint32_t gen_ = 0; // bumped on every Recalc; guards stale server previews

public:
  // Server-driven preview provider: fired when the grid changes so the owning
  // window can issue a server grid-check. Left empty until wired by the
  // CraftingWindow (no local recipe knowledge on the client anymore).
  std::function<void(const std::array<ItemStack, 9> &)> onGridChanged_;

  CraftingGrid() {
    slots_.setOnChange([this](int, ItemStack, ItemStack) { Recalc(); });
  }

  const SlotContainer<9, ItemStack> &Slots() const { return slots_; }
  const ItemStack &GetResult() const { return result_; }

  // Generation of the current grid state (for matching async replies).
  uint32_t Generation() const { return gen_; }

  // Apply a server grid-check result, but only if it corresponds to the
  // current grid generation (stale replies are dropped).
  void ApplyServerResult(uint32_t gen, const ItemStack &result) {
    if (gen == gen_) result_ = result;
  }

  // Override result (used by OnCraftResponse to show crafted item).
  void SetResult(const ItemStack &result) { result_ = result; }

  // Bump generation so any in-flight server preview is treated as stale
  // (e.g. after a craft result replaces the grid contents).
  void InvalidatePreview() { ++gen_; }

  // Set all 9 grid slots from server response (consumed grid after crafting).
  // Suppresses onGridChanged_ to prevent a stale async preview from wiping
  // the result that OnCraftResponse is about to set.
  void SetSlots(const std::array<ItemStack, 9> &slots);

  // Clear all slots + result
  void Clear();

  // Recalculate result via the server preview provider (async)
  void Recalc();
};
