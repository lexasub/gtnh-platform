#include "UI/Components/CraftingGrid.h"
#include "UI/Core/DragManager.h"
#include "Crafting/ClientRecipeDB.h"

void CraftingGrid::HandleActivate(int gridIdx, InventoryState& inv, DragManager& dm) {
    if (gridIdx < 0 || gridIdx >= 9) return;

    if (!dm.IsDragging()) {
        if (slots_[gridIdx].item_id == 0) return;
        dm.StartExternalDrag(kGridFlag + gridIdx, slots_[gridIdx]);
        slots_.set(gridIdx, ItemStack{});
        return;
    }

    bool srcIsGrid = dm.GetSourceSlot() >= kGridFlag;

    if (srcIsGrid && dm.GetSourceSlot() == kGridFlag + gridIdx) {
        slots_.set(gridIdx, dm.GetHeldItem());
        dm.Reset();
        return;
    }

    if (srcIsGrid) {
        std::vector<ItemStack> gridVec(slots_.begin(), slots_.end());
        dm.OnSlotActivated(gridIdx, gridVec, 0, false);
        for (int i = 0; i < 9; ++i) slots_.set(i, gridVec[i]);
    } else {
        ItemStack target = slots_[gridIdx];
        ItemStack held = dm.GetHeldItem();
        if (target.item_id == held.item_id && target.meta == held.meta && target.count < 64) {
            int space = 64 - target.count;
            int toAdd = std::min(space, (int)held.count);
            target.count += toAdd;
            held.count -= toAdd;
            slots_.set(gridIdx, target);
            if (held.count == 0) {
                dm.Reset();
            } else {
                dm.StartExternalDrag(dm.GetSourceSlot(), held);
            }
        } else if (target.item_id == 0) {
            slots_.set(gridIdx, held);
            dm.Reset();
        } else {
            slots_.set(gridIdx, held);
            inv.slots[dm.GetSourceSlot()] = target;
            dm.Reset();
        }
    }
}

void CraftingGrid::HandleCancel(DragManager& dm) {
    if (!dm.IsDragging()) return;
    int srcIdx = dm.GetSourceSlot();
    if (srcIdx >= kGridFlag) {
        int gridIdx = srcIdx - kGridFlag;
        if (gridIdx >= 0 && gridIdx < 9) {
            slots_.set(gridIdx, dm.GetHeldItem());
        }
    }
    dm.Reset();
}

void CraftingGrid::Clear() {
    slots_.clear();
    result_ = ItemStack{};
}

void CraftingGrid::SetSlots(const std::array<ItemStack, 9>& slots) {
    slots_.setAll(slots);
    result_ = ItemStack{};
}

void CraftingGrid::Recalc() {
    Crafting::MatchGrid(slots_.data(), &result_);
}
