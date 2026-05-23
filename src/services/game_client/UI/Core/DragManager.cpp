#include "UI/Core/DragManager.h"
#include <algorithm>

DragManager::ActionResult DragManager::OnSlotActivated(int slotIndex, std::vector<ItemStack>& slots, int button, bool shift) {
    (void)button;
    (void)shift;
    ActionResult r;

    if (state_ == State::Idle) {
        if (slotIndex < 0 || static_cast<size_t>(slotIndex) >= slots.size()) return r;
        auto& slot = slots[slotIndex];
        if (slot.item_id == 0) return r;
        heldItem_ = slot;
        sourceSlot_ = slotIndex;
        slot = ItemStack{};
        state_ = State::Holding;
        r.consumed = true;
        r.isDraggingAfter = true;
        r.sourceSlot = sourceSlot_;
        r.item = heldItem_;
        r.count = heldItem_.count;
        return r;
    }

    if (slotIndex < 0 || static_cast<size_t>(slotIndex) >= slots.size()) return r;
    auto& slot = slots[slotIndex];
    r.consumed = true;
    r.sourceSlot = sourceSlot_;

    if (slotIndex == sourceSlot_) {
        slot = heldItem_;
        heldItem_ = ItemStack{};
        sourceSlot_ = -1;
        state_ = State::Idle;
        return r;
    }

    if (slot.item_id == heldItem_.item_id && slot.meta == heldItem_.meta && slot.count < 64) {
        uint8_t space = 64 - slot.count;
        uint8_t toAdd = std::min(space, heldItem_.count);
        slot.count += toAdd;
        r.targetSlot = slotIndex;
        r.item = heldItem_;
        r.count = toAdd;
        if (cb_) cb_(0, sourceSlot_, slotIndex, toAdd);
        heldItem_.count -= toAdd;
        if (heldItem_.count == 0) {
            heldItem_ = ItemStack{};
            sourceSlot_ = -1;
            state_ = State::Idle;
        }
        return r;
    }

    if (slot.item_id == 0) {
        slot = heldItem_;
        r.targetSlot = slotIndex;
        r.item = heldItem_;
        r.count = heldItem_.count;
        if (cb_) cb_(0, sourceSlot_, slotIndex, heldItem_.count);
        heldItem_ = ItemStack{};
        sourceSlot_ = -1;
        state_ = State::Idle;
        return r;
    }

    ItemStack tmp = slot;
    slot = heldItem_;
    if (sourceSlot_ >= 0 && static_cast<size_t>(sourceSlot_) < slots.size()) {
        slots[sourceSlot_] = tmp;
    }
    r.targetSlot = slotIndex;
    r.item = heldItem_;
    r.count = heldItem_.count;
    if (cb_) cb_(0, sourceSlot_, slotIndex, heldItem_.count);
    heldItem_ = ItemStack{};
    sourceSlot_ = -1;
    state_ = State::Idle;
    return r;
}

void DragManager::CancelDrag(std::vector<ItemStack>& slots) {
    if (state_ != State::Holding) return;
    if (sourceSlot_ >= 0 && static_cast<size_t>(sourceSlot_) < slots.size()) {
        slots[sourceSlot_] = heldItem_;
    } else {
        // source is outside slots vector (e.g. grid → inventory cancel);
        // caller must return item to source manually.
    }
    heldItem_ = ItemStack{};
    sourceSlot_ = -1;
    hoverSlot_ = -1;
    state_ = State::Idle;
}

void DragManager::UpdateHover(int slotIndex) {
    hoverSlot_ = slotIndex;
}

void DragManager::RenderPreview(const SlotStyle&) {}

void DragManager::DropHeldItem() {
    if (state_ != State::Holding) return;
    if (cb_) {
        cb_(kActionDrop, static_cast<uint8_t>(sourceSlot_), 255, heldItem_.count);
    }
    heldItem_ = ItemStack{};
    sourceSlot_ = -1;
    hoverSlot_ = -1;
    state_ = State::Idle;
}

void DragManager::StartExternalDrag(int sourceSlot, const ItemStack& item) {
    heldItem_ = item;
    sourceSlot_ = sourceSlot;
    state_ = State::Holding;
}

void DragManager::Reset() {
    heldItem_ = ItemStack{};
    sourceSlot_ = -1;
    hoverSlot_ = -1;
    state_ = State::Idle;
    ClearMachineDragContext();
}

void DragManager::SyncTo(InventoryState& inv) const {
    inv.isDragging = IsDragging();
    inv.dragItem = GetHeldItem();
    inv.dragSourceSlot = GetSourceSlot();
    inv.dragHoverSlot = GetHoverSlot();
}

void DragManager::SyncFrom(const InventoryState& inv) {
    if (inv.isDragging) {
        heldItem_ = inv.dragItem;
        sourceSlot_ = inv.dragSourceSlot;
        hoverSlot_ = inv.dragHoverSlot;
        state_ = State::Holding;
    } else {
        Reset();
    }
}
