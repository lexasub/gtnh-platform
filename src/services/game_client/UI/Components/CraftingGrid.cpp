#include "UI/Components/CraftingGrid.h"

void CraftingGrid::Clear() {
    ++gen_;
    slots_.clear();
    result_ = ItemStack{};
}

void CraftingGrid::SetSlots(const std::array<ItemStack, 9>& slots) {
    // Suppress onGridChanged_ while bulk-loading server state; otherwise the
    // change notification fires Recalc() → server preview request whose reply
    // can arrive after OnCraftResponse has set the result and wipe it.
    auto saved = slots_.getOnChange();  // save current callback
    slots_.setOnChange(nullptr);        // suppress
    slots_.setAll(slots);
    slots_.setOnChange(std::move(saved));  // restore
    result_ = ItemStack{};
}

void CraftingGrid::Recalc() {
    ++gen_;
    if (onGridChanged_) {
        // The owning window issues a server grid-check; the reply comes back
        // via ApplyServerResult(gen, output).
        onGridChanged_(slots_.data());
    } else {
        result_ = ItemStack{};
    }
}
