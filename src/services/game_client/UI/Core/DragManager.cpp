#include "UI/Core/DragManager.h"
#include "UI/Components/SlotGrid.h"
#include "RenderLib/Utils/TextureAtlas.h"
#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <spdlog/spdlog.h>

// ── Action handling ──────────────────────────────────────────────────────

DragManager::ActionResult DragManager::OnSlotActivated(int slotIndex,
    std::vector<ItemStack>& slots, int button, bool shift, bool ctrl,
    int reportedSlotIndex) {
    ActionResult r;
    if (slotIndex < 0 || static_cast<size_t>(slotIndex) >= slots.size()) return r;
    if (reportedSlotIndex < 0) reportedSlotIndex = slotIndex;

    if (state_ == State::Idle) {
        auto& slot = slots[slotIndex];
        if (slot.item_id == 0) return r;

        // ── Shift/Ctrl-click: quick-move, no drag ────────────────────────
        if (shift || ctrl) {
            r.consumed = true;
            r.sourceSlot = reportedSlotIndex;
            r.item = slot;
            r.count = slot.count;
            if (cb_) cb_(kActionQuickMove, static_cast<uint8_t>(reportedSlotIndex), 255, slot.count);
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
            reportedSourceSlot_ = reportedSlotIndex;
            state_ = State::Holding;
            r.consumed = true;
            r.isDraggingAfter = true;
            r.sourceSlot = reportedSlotIndex;
            r.item = heldItem_;
            r.count = half;
            if (cb_) cb_(kActionSplit, static_cast<uint8_t>(reportedSlotIndex), 255, half);
            return r;
        }

        // ── Left-click: normal pickup ─────────────────────────────────────
        heldItem_ = slot;
        sourceSlot_ = slotIndex;
        reportedSourceSlot_ = reportedSlotIndex;
        slot = ItemStack{};
        state_ = State::Holding;
        r.consumed = true;
        r.isDraggingAfter = true;
        r.sourceSlot = reportedSlotIndex;
        r.item = heldItem_;
        r.count = heldItem_.count;
        return r;
    }

    // ── state_ == State::Holding ─────────────────────────────────────────
    if (slotIndex < 0 || static_cast<size_t>(slotIndex) >= slots.size()) return r;
    auto& slot = slots[slotIndex];
    r.consumed = true;
    r.sourceSlot = reportedSourceSlot_;

    // ── Shift while holding: quick-move ──────────────────────────────────
    if (shift) {
        r.item = heldItem_;
        r.count = heldItem_.count;
        r.targetSlot = slotIndex;
        if (cb_) cb_(kActionQuickMove, static_cast<uint8_t>(reportedSourceSlot_), slotIndex, heldItem_.count);
        // Return item to source — server will do the actual move
        if (sourceSlot_ >= 0 && static_cast<size_t>(sourceSlot_) < slots.size()) {
            slots[sourceSlot_] = heldItem_;
        }
        heldItem_ = ItemStack{};
        reportedSourceSlot_ = -1;
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
            reportedSourceSlot_ = -1;
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
            if (cb_) cb_(kActionMove, static_cast<uint8_t>(reportedSourceSlot_), slotIndex, 1);
            heldItem_.count -= 1;
            if (heldItem_.count == 0) {
                heldItem_ = ItemStack{};
                reportedSourceSlot_ = -1;
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
            if (cb_) cb_(kActionMove, static_cast<uint8_t>(reportedSourceSlot_), slotIndex, 1);
            heldItem_.count -= 1;
            if (heldItem_.count == 0) {
                heldItem_ = ItemStack{};
                reportedSourceSlot_ = -1;
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
        reportedSourceSlot_ = -1;
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
        if (cb_) cb_(kActionMove, static_cast<uint8_t>(reportedSourceSlot_), slotIndex, toAdd);
        heldItem_.count -= toAdd;
        if (heldItem_.count == 0) {
            heldItem_ = ItemStack{};
            reportedSourceSlot_ = -1;
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
        if (cb_) cb_(kActionMove, static_cast<uint8_t>(reportedSourceSlot_), slotIndex, heldItem_.count);
        heldItem_ = ItemStack{};
        reportedSourceSlot_ = -1;
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
    if (cb_) cb_(kActionMove, static_cast<uint8_t>(reportedSourceSlot_), slotIndex, heldItem_.count);
    heldItem_ = ItemStack{};
    reportedSourceSlot_ = -1;
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
    reportedSourceSlot_ = -1;
    sourceSlot_ = -1;
    hoverSlot_ = -1;
    state_ = State::Idle;
}

void DragManager::UpdateHover(int slotIndex) {
  hoverSlot_ = slotIndex;
}

void DragManager::OnRightDragDistribute(int slotIndex, std::vector<ItemStack> &slots) {
  if (state_ != State::Holding || heldItem_.count == 0) return;
  if (slotIndex < 0 || slotIndex >= static_cast<int>(slots.size())) return;

  auto &slot = slots[slotIndex];
  if (slot.item_id == 0) {
    slot = {heldItem_.item_id, 1, heldItem_.meta};
    heldItem_.count--;
  } else if (slot.item_id == heldItem_.item_id && slot.meta == heldItem_.meta && slot.count < 64) {
    slot.count++;
    heldItem_.count--;
  }

  if (heldItem_.count == 0) {
    state_ = State::Idle;
    heldItem_ = {};
  }
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
        cb_(kActionDrop, static_cast<uint8_t>(reportedSourceSlot_), 255, heldItem_.count);
    }
    heldItem_ = ItemStack{};
    reportedSourceSlot_ = -1;
    sourceSlot_ = -1;
    hoverSlot_ = -1;
    state_ = State::Idle;
}

// ── Authoritative click path (server owns the cursor) ─────────────────────
// No local slot mutation — the gesture is translated to a click descriptor
// and the server publishes the authoritative snapshot back to the client.

void DragManager::OnPlayerSlotClick(int slotIndex, int button, bool shift, bool ctrl) {
    if (slotIndex < 0 || slotIndex >= kPlayerSlots) return;
    ClickInfo info;
    info.actionType = (shift || ctrl) ? kClickActionQuickMove : kClickActionClick;
    info.button = static_cast<uint8_t>(button);
    info.mods = static_cast<uint8_t>((shift ? kModShiftBit : 0) | (ctrl ? kModCtrlBit : 0));
    info.containerId = 0; // player inventory
    info.slot = static_cast<uint16_t>(slotIndex);
    if (clickCb_) clickCb_(info);
}

void DragManager::OnPlayerDrop(int slotIndex) {
    if (slotIndex < 0 || slotIndex >= kPlayerSlots) return;
    ClickInfo info;
    info.actionType = kClickActionDrop;
    info.containerId = 0;
    info.slot = static_cast<uint16_t>(slotIndex);
    if (clickCb_) clickCb_(info);
}

void DragManager::OnPlayerDragPlace(int slotIndex) {
    if (slotIndex < 0 || slotIndex >= kPlayerSlots) return;
    ClickInfo info;
    info.actionType = kClickActionDragPlace;
    info.containerId = 0;
    info.count = 1;
    info.slot = static_cast<uint16_t>(slotIndex);
    if (clickCb_) clickCb_(info);
}

void DragManager::OnContainerSlotClick(int slotIndex, uint8_t containerId,
                                       int button, bool shift, bool ctrl) {
    ClickInfo info;
    info.actionType = (shift || ctrl) ? kClickActionQuickMove : kClickActionClick;
    info.button = static_cast<uint8_t>(button);
    info.mods = static_cast<uint8_t>((shift ? kModShiftBit : 0) | (ctrl ? kModCtrlBit : 0));
    info.containerId = containerId;
    info.slot = static_cast<uint16_t>(slotIndex);
    if (clickCb_) clickCb_(info);
}

void DragManager::OnContainerDrop(int slotIndex, uint8_t containerId) {
    ClickInfo info;
    info.actionType = kClickActionDrop;
    info.containerId = containerId;
    info.slot = static_cast<uint16_t>(slotIndex);
    if (clickCb_) clickCb_(info);
}

void DragManager::OnContainerDragPlace(int slotIndex, uint8_t containerId) {
    ClickInfo info;
    info.actionType = kClickActionDragPlace;
    info.containerId = containerId;
    info.count = 1;
    info.slot = static_cast<uint16_t>(slotIndex);
    if (clickCb_) clickCb_(info);
}

void DragManager::StartExternalDrag(int sourceSlot, const ItemStack& item) {
    heldItem_ = item;
    sourceSlot_ = sourceSlot;
    reportedSourceSlot_ = sourceSlot;
    state_ = State::Holding;
}

void DragManager::Reset() {
    heldItem_ = ItemStack{};
    sourceSlot_ = -1;
    reportedSourceSlot_ = -1;
    hoverSlot_ = -1;
    state_ = State::Idle;
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
        reportedSourceSlot_ = inv.dragSourceSlot;
        hoverSlot_ = inv.dragHoverSlot;
        state_ = State::Holding;
    } else {
        Reset();
    }
}
