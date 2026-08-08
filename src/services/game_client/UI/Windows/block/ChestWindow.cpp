#include "ChestWindow.h"
#include "Common/Inventory.h"
#include "Components/SlotGrid.h"
#include "UI/Core/DragManager.h"
#include "Network/NetClient.h"
#include "RenderLib/Utils/TextureAtlas.h"
#include "core_generated.h"
#include <imgui.h>
#include <cstdio>
#include <functional>
#include <spdlog/spdlog.h>

ChestWindow::ChestWindow(BlockPos pos)
    : BlockAttachedWindow(pos)
    , open_(false)
    , chestSlots_(27) {}

void ChestWindow::SetOpen(bool open) {
    if (open && !open_) {
        // Slots are unloaded until BlockEntityUpdate arrives — saving before that wipes server state.
        chestSlots_.assign(27, ItemStack{});
        dataLoaded_ = false;
        heldItem_ = ItemStack{};
        heldFromSlot_ = -1;
    }
    if (!open && open_ && netClient_ && lastPlayerInv_) {
        if (dataLoaded_) {
            CommitHeldItem(lastPlayerInv_);
            netClient_->SendChestSaveReq(pos_, chestSlots_, lastPlayerInv_->slots,
                                         lastPlayerInv_->player_id);
        } else {
            spdlog::warn("[Chest] Close before BlockEntityUpdate at ({},{},{}) — skipped save",
                         pos_.x, pos_.y, pos_.z);
        }
        dataLoaded_ = false;
    }
    open_ = open;
}

void ChestWindow::SaveState(InventoryState* playerInv) {
    if (netClient_ && playerInv && dataLoaded_) {
        CommitHeldItem(playerInv);
        netClient_->SendChestSaveReq(pos_, chestSlots_, playerInv->slots, playerInv->player_id);
    }
}

void ChestWindow::OnNetworkUpdate(uint8_t msgType, const void* data) {
    if (msgType != GatewayMsg::kBlockEntityUpdate) return;
    if (!data) return;

    flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(data), 8192);
    if (!v.VerifyBuffer<Protocol::BlockEntityUpdate>(nullptr)) return;

    auto* update = flatbuffers::GetRoot<Protocol::BlockEntityUpdate>(data);
    auto* updatePos = update->pos();
    if (!updatePos || updatePos->x() != pos_.x || updatePos->y() != pos_.y || updatePos->z() != pos_.z) return;

    if (auto* inItems = update->input_items()) {
        chestSlots_.assign(27, ItemStack{});
        size_t n = std::min(static_cast<size_t>(inItems->size()), chestSlots_.size());
        for (size_t i = 0; i < n; ++i) {
            auto* s = inItems->Get(i);
            if (s) chestSlots_[i] = {static_cast<uint16_t>(s->item_id()), s->count(), static_cast<uint16_t>(s->meta())};
        }
        dataLoaded_ = true;
    }
}

// ── Utility helpers ──────────────────────────────────────────────────────────

ItemStack ChestWindow::PlaceIntoInventory(ItemStack item, InventoryState* playerInv) {
    if (item.item_id == 0) return item;
    for (auto& slot : playerInv->slots) {
        if (slot.item_id == item.item_id && slot.meta == item.meta && slot.count < 64) {
            uint8_t space = 64 - slot.count;
            uint8_t a = std::min(space, item.count);
            slot.count += a;
            item.count -= a;
            if (item.count == 0) return ItemStack{};
        }
    }
    for (auto& slot : playerInv->slots) {
        if (slot.item_id == 0) { slot = item; return ItemStack{}; }
    }
    return item; // inventory full
}

void ChestWindow::QuickMoveToInv(int slot, std::vector<ItemStack>& chestSlots, InventoryState* playerInv) {
    if (slot < 0 || static_cast<size_t>(slot) >= chestSlots.size()) return;
    if (chestSlots[slot].item_id == 0) return;
    chestSlots[slot] = PlaceIntoInventory(chestSlots[slot], playerInv);
}

void ChestWindow::QuickMoveToChest(int invSlot, std::vector<ItemStack>& chestSlots, InventoryState* playerInv) {
    if (invSlot < 0 || static_cast<size_t>(invSlot) >= playerInv->slots.size()) return;
    if (playerInv->slots[invSlot].item_id == 0) return;
    ItemStack item = playerInv->slots[invSlot];
    playerInv->slots[invSlot] = ItemStack{};
    // Try merge first, then empty slot
    for (auto& cs : chestSlots) {
        if (cs.item_id == item.item_id && cs.meta == item.meta && cs.count < 64) {
            uint8_t space = 64 - cs.count;
            uint8_t a = std::min(space, item.count);
            cs.count += a; item.count -= a;
            if (item.count == 0) return;
        }
    }
    for (auto& cs : chestSlots) {
        if (cs.item_id == 0) { cs = item; return; }
    }
    // Chest full — put it back
    for (auto& is : playerInv->slots) {
        if (is.item_id == item.item_id && is.meta == item.meta && is.count < 64) {
            uint8_t space = 64 - is.count;
            uint8_t a = std::min(space, item.count);
            is.count += a; item.count -= a;
            if (item.count == 0) return;
        }
    }
    for (auto& is : playerInv->slots) {
        if (is.item_id == 0) { is = item; return; }
    }
}

// ── Slot click handler — both grids share the same local-held_ state ───────
// gridId: 0 = chest slots, 1 = player inventory
static void OnGridSlotClicked(int slot, int /*button*/, bool /*shift*/,
                               int gridId, ChestWindow* win,
                               std::vector<ItemStack>& chestSlots,
                               InventoryState* playerInv) {
    ItemStack& held = win->heldItem_;
    int& fromSlot = win->heldFromSlot_;
    if (slot < 0) return;

    // ── Quick-move (shift-click) ────────────────────────────────────────
    if (ImGui::GetIO().KeyShift) {
        if (held.item_id == 0 && fromSlot < 0) {
            if (gridId == 0) ChestWindow::QuickMoveToInv(slot, chestSlots, playerInv);
            else ChestWindow::QuickMoveToChest(slot, chestSlots, playerInv);
        }
        return;
    }

    // ── Holding an item: drop or swap ────────────────────────────────────
    if (held.item_id != 0) {
        auto& dst = (gridId == 0) ? chestSlots[slot] : playerInv->slots[slot];

        if (dst.item_id == held.item_id && dst.meta == held.meta && dst.count < 64) {
            // Merge
            uint8_t space = 64 - dst.count;
            uint8_t a = std::min(space, held.count);
            dst.count += a;
            held.count -= a;
        } else {
            // Swap
            ItemStack tmp = dst;
            dst = held;
            held = tmp;
            // If we swapped back to self, just clear
        }

        if (held.count == 0) { held = ItemStack{}; fromSlot = -1; }
        return;
    }

    // ── No item held: pick up ────────────────────────────────────────────
    auto& src = (gridId == 0) ? chestSlots[slot] : playerInv->slots[slot];
    if (src.item_id == 0) return;

    // Right-click: pick up half
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        uint8_t half = (src.count + 1) / 2;
        held = src;
        held.count = half;
        src.count -= half;
        if (src.count == 0) src = ItemStack{};
        fromSlot = gridId == 0 ? slot : (100 + slot);
    } else {
        // Left-click: pick up whole stack
        held = src;
        src = ItemStack{};
        fromSlot = gridId == 0 ? slot : (100 + slot);
    }
}

void ChestWindow::CommitHeldItem(InventoryState* playerInv) {
    if (heldItem_.item_id == 0) { heldFromSlot_ = -1; return; }
    ItemStack item = heldItem_;
    auto placeBack = [&](auto& slots) {
        for (auto& s : slots) {
            if (s.item_id == item.item_id && s.meta == item.meta && s.count < 64) {
                uint8_t a = std::min(static_cast<uint8_t>(64 - s.count), item.count);
                s.count += a; item.count -= a;
                if (item.count == 0) return true;
            }
        }
        for (auto& s : slots) {
            if (s.item_id == 0) { s = item; item = ItemStack{}; return true; }
        }
        return false;
    };
    if (heldFromSlot_ >= 0 && heldFromSlot_ < 27) {
        auto& src = chestSlots_[heldFromSlot_];
        if (src.item_id == 0) { src = item; item = ItemStack{}; }
        else if (src.item_id == item.item_id && src.meta == item.meta && src.count < 64) {
            uint8_t a = std::min(static_cast<uint8_t>(64 - src.count), item.count);
            src.count += a; item.count -= a;
        }
    } else if (heldFromSlot_ >= 100 && playerInv) {
        int is = heldFromSlot_ - 100;
        if (is >= 0 && is < static_cast<int>(playerInv->slots.size())) {
            auto& src = playerInv->slots[is];
            if (src.item_id == 0) { src = item; item = ItemStack{}; }
            else if (src.item_id == item.item_id && src.meta == item.meta && src.count < 64) {
                uint8_t a = std::min(static_cast<uint8_t>(64 - src.count), item.count);
                src.count += a; item.count -= a;
            }
        }
    }
    if (item.item_id != 0 && playerInv && !placeBack(playerInv->slots)) {
        placeBack(chestSlots_);
    }
    heldItem_ = ItemStack{};
    heldFromSlot_ = -1;
}

void ChestWindow::Render(InventoryState* playerInv) {
    lastPlayerInv_ = playerInv;
    if (!open_) return;

    // Return held item to source if window respawned with stale drag
    if (heldItem_.item_id != 0 && heldFromSlot_ < 0) {
        heldItem_ = ItemStack{};
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);
    ImGui::Begin("Chest", nullptr);

    // ─── Chest storage ───────────────────────────────────────────────
    ImGui::Text("Chest Storage");
    ImGui::Separator();

    {
        SlotStyle style;
        style.size = 40;
        style.padding = 2;
        style.showNumbers = true;
        style.drawBackground = true;

        std::function<void(int, int, bool)> cb = [&](int slot, int btn, bool sh) {
            OnGridSlotClicked(slot, btn, sh, 0, this, chestSlots_, playerInv);
        };
        RenderSlotGrid(chestSlots_, 0, static_cast<int>(chestSlots_.size()), 9, -1, style, &cb);
    }

    // ─── Visual divider + Player inventory label ─────────────────────
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::Text("Player Inventory");
    ImGui::Separator();

    ImGui::PushID("player_inv");
    {
        SlotStyle style;
        style.size = 40;
        style.padding = 2;
        style.showNumbers = true;
        style.drawBackground = true;

        std::function<void(int, int, bool)> cb = [&](int slot, int btn, bool sh) {
            OnGridSlotClicked(slot, btn, sh, 1, this, chestSlots_, playerInv);
        };
        RenderSlotGrid(playerInv->slots, 0, static_cast<int>(playerInv->slots.size()), 9, -1, style, &cb);
    }
    ImGui::PopID();

    // ─── Render held item under cursor ────────────────────────────────
    if (heldItem_.item_id != 0) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 mpos = ImGui::GetIO().MousePos;
        float sz = 36.0f;
        auto uv = renderlib::TextureAtlas::GetItemUV(heldItem_.item_id);
        dl->AddImage(
            ImTextureID(static_cast<ImTextureID>(renderlib::TextureAtlas::GetTextureHandle().idx)),
            ImVec2(mpos.x + 4, mpos.y + 4),
            ImVec2(mpos.x + sz, mpos.y + sz),
            ImVec2(uv.u0, uv.v0), ImVec2(uv.u1, uv.v1));
        if (heldItem_.count > 1) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", heldItem_.count);
            dl->AddText(ImVec2(mpos.x + 4, mpos.y + 4), IM_COL32(255,255,255,255), buf);
        }
    }

    // ─── ESC cancels held item ────────────────────────────────────────
    if (heldItem_.item_id != 0 && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        CommitHeldItem(playerInv);
    }

    ImGui::End();
}

bool ChestWindow::OnKeyEvent(int /*key*/, int /*action*/, int /*mods*/) {
    if (!open_) return false;
    return false;
}
