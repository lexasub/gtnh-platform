#include "UI/Core/DragManager.h"
#include "UI/Components/SlotGrid.h"
#include "RenderLib/Utils/TextureAtlas.h"
#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <spdlog/spdlog.h>

// ── Action handling ──────────────────────────────────────────────────────

DragManager::ActionResult DragManager::OnSlotActivated(int slotIndex,
    std::vector<ItemStack>& slots, int button, bool shift) {
    ActionResult r;
    if (slotIndex < 0 || static_cast<size_t>(slotIndex) >= slots.size()) return r;


    // Notify machine window about slot activation if drag completes (Holding→Idle).
    // Only fire while actually holding an item — not during initial pickup (Idle state).
    if (hasMachineDrag_ && machineCb_ && state_ == State::Holding) {
        spdlog::info("DragManager: Machine slot {} activated, sending SET_MACHINE_SLOT_REQ", slotIndex);
        machineCb_(kActionMove, static_cast<uint8_t>(machineDragSlotIdx_),
                   static_cast<uint8_t>(slotIndex), 0, machineDragPos_);
    }

    if (state_ == State::Idle) {
        auto& slot = slots[slotIndex];
        if (slot.item_id == 0) return r;

        // ── Shift-click: quick-move, no drag ─────────────────────────────
        if (shift) {
            r.consumed = true;
            r.sourceSlot = slotIndex;
            r.item = slot;
            r.count = slot.count;
            if (cb_) cb_(kActionQuickMove, slotIndex, 255, slot.count);
            // Don't touch slots — caller/network will update
            return r;
        }

        // ── Right-click: pick up half ─────────────────────────────────────
        if (button == 1) {
            uint8_t half = (slot.count + 1) / 2; // ceil, so right-click on 1 → 1
            heldItem_ = slot;
            heldItem_.count = half;
            slot.count -= half;
            if (slot.count == 0) slot = ItemStack{};
            sourceSlot_ = slotIndex;
            state_ = State::Holding;
            r.consumed = true;
            r.isDraggingAfter = true;
            r.sourceSlot = slotIndex;
            r.item = heldItem_;
            r.count = half;
            if (cb_) cb_(kActionSplit, slotIndex, 255, half);
            return r;
        }

        // ── Left-click: normal pickup ─────────────────────────────────────
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

    // ── state_ == State::Holding ─────────────────────────────────────────
    if (slotIndex < 0 || static_cast<size_t>(slotIndex) >= slots.size()) return r;
    auto& slot = slots[slotIndex];
    r.consumed = true;
    r.sourceSlot = sourceSlot_;

    // ── Shift while holding: quick-move ──────────────────────────────────
    if (shift) {
        r.item = heldItem_;
        r.count = heldItem_.count;
        r.targetSlot = slotIndex;
        if (cb_) cb_(kActionQuickMove, sourceSlot_, slotIndex, heldItem_.count);
        // Return item to source — server will do the actual move
        if (sourceSlot_ >= 0 && static_cast<size_t>(sourceSlot_) < slots.size()) {
            slots[sourceSlot_] = heldItem_;
        }
        heldItem_ = ItemStack{};
        sourceSlot_ = -1;
        state_ = State::Idle;
        return r;
    }

    // ── Right-click while holding: place 1 (drag-to-distribute) ──────────
    if (button == 1) {
        if (slotIndex == sourceSlot_) {
            // Clicking source: return all (same as left-click on source)
            slot = heldItem_;
            heldItem_ = ItemStack{};
            sourceSlot_ = -1;
            state_ = State::Idle;
            return r;
        }

        if (slot.item_id == 0) {
            // Empty slot: place 1
            slot = {heldItem_.item_id, 1, heldItem_.meta};
            r.targetSlot = slotIndex;
            r.item = heldItem_;
            r.count = 1;
            if (cb_) cb_(kActionMove, sourceSlot_, slotIndex, 1);
            heldItem_.count -= 1;
            if (heldItem_.count == 0) {
                heldItem_ = ItemStack{};
                sourceSlot_ = -1;
                state_ = State::Idle;
            }
            return r;
        }

        if (slot.item_id == heldItem_.item_id && slot.meta == heldItem_.meta
            && slot.count < 64) {
            // Same item, not full: place 1
            slot.count += 1;
            r.targetSlot = slotIndex;
            r.item = heldItem_;
            r.count = 1;
            if (cb_) cb_(kActionMove, sourceSlot_, slotIndex, 1);
            heldItem_.count -= 1;
            if (heldItem_.count == 0) {
                heldItem_ = ItemStack{};
                sourceSlot_ = -1;
                state_ = State::Idle;
            }
            return r;
        }

        // Different item or full slot: no-op on right-click
        return r;
    }

    // ── Left-click while holding ────────────────────────────────────────
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
        if (cb_) cb_(kActionMove, sourceSlot_, slotIndex, toAdd);
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
        if (cb_) cb_(kActionMove, sourceSlot_, slotIndex, heldItem_.count);
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
    if (cb_) cb_(kActionMove, sourceSlot_, slotIndex, heldItem_.count);
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

void DragManager::RenderPreview(const SlotStyle& style) {
    if (state_ != State::Holding) return;
    if (heldItem_.item_id == 0) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 mousePos = ImGui::GetIO().MousePos;
    int sz = style.size;

    auto uv = renderlib::TextureAtlas::GetItemUV(heldItem_.item_id);
    dl->AddImage(
        ImTextureID(static_cast<ImTextureID>(renderlib::TextureAtlas::GetTextureHandle().idx)),
        ImVec2(mousePos.x + 4, mousePos.y + 4),
        ImVec2(mousePos.x + sz - 4, mousePos.y + sz - 4),
        ImVec2(uv.u0, uv.v0),
        ImVec2(uv.u1, uv.v1)
    );

    if (style.showNumbers && heldItem_.count > 1) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", heldItem_.count);
        dl->AddText(ImVec2(mousePos.x + 4, mousePos.y + 4),
                    IM_COL32(255, 255, 255, 255), buf);
    }
}

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

void DragManager::OnMachineSlotAck(uint8_t slotIdx, bool success) {
    spdlog::info("DragManager: Machine slot {} ack success={}", slotIdx, success);
    if (machineSlotAckCb_) {
        machineSlotAckCb_(slotIdx, success);
    }
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
