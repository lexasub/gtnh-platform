#include "ChestWindow.h"
#include "Common/Inventory.h"
#include "Components/SlotGrid.h"
#include "Components/PlayerInventoryGrid.h"
#include "UI/Core/DragManager.h"
#include "Network/NetClient.h"
#include "RenderLib/Utils/TextureAtlas.h"
#include "core_generated.h"
#include <imgui.h>
#include <cstdio>
#include <spdlog/spdlog.h>

ChestWindow::ChestWindow(BlockPos pos)
    : BlockAttachedWindow(pos)
    , open_(false)
    , chestSlots_(27) {}

void ChestWindow::SetOpen(bool open) {
    if (open && !open_) {
        // Slots are unloaded until the container_id=1 snapshot arrives —
        // clicks are gated server-side (OH3), and dataLoaded_ guards close.
        chestSlots_.assign(27, ItemStack{});
        dataLoaded_ = false;
        if (netClient_ && player_id_ != 0) {
            netClient_->SendChestOpenReq(player_id_, pos_);
        }
    }
    if (!open && open_) {
        if (netClient_ && player_id_ != 0) {
            netClient_->SendChestCloseReq(player_id_, pos_);
        }
        dataLoaded_ = false;
    }
    open_ = open;
}

void ChestWindow::OnNetworkUpdate(uint8_t msgType, const void* data) {
    if (msgType == GatewayMsg::kInventoryUpdate) {
        if (!data) return;
        flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(data), 8192);
        if (!v.VerifyBuffer<Protocol::InventoryUpdate>(nullptr)) return;
        auto* update = flatbuffers::GetRoot<Protocol::InventoryUpdate>(data);
        if (update->container_id() != 1) return; // ignore player-only (container_id=0) snapshots
        auto* cp = update->container_pos();
        if (!cp || cp->x() != pos_.x || cp->y() != pos_.y || cp->z() != pos_.z) return;
        auto* cs = update->container_slots();
        if (!cs) return;
        chestSlots_.assign(27, ItemStack{});
        size_t n = std::min(static_cast<size_t>(cs->size()), chestSlots_.size());
        for (size_t i = 0; i < n; ++i) {
            auto* s = cs->Get(i);
            if (s) chestSlots_[i] = {s->item_id(), s->count(), s->meta()};
        }
        dataLoaded_ = true;
        return;
    }

    // Machines still come through BlockEntityUpdate; chests no longer do.
    IUIWindow::OnNetworkUpdate(msgType, data);
}

void ChestWindow::Render(InventoryState* playerInv) {
    if (!open_) return;

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);
    ImGui::Begin("Chest", nullptr);

    // ─── Chest storage (container_id=1, authoritative clicks) ────────────
    ImGui::Text("Chest Storage");
    ImGui::Separator();

    {
        SlotStyle style;
        style.size = 40;
        style.padding = 2;
        style.showNumbers = true;
        style.drawBackground = true;

        SlotGridComponent chestGrid(chestSlots_);
        chestGrid.SetStyle(style);
        chestGrid.SetRange(0, static_cast<int>(chestSlots_.size()), 9);
        chestGrid.SetSlotIndexOffset(DragManager::kChestSlotBase);
        chestGrid.SetDragManager(dragMgr_);
        chestGrid.SetInventory(*playerInv);
        chestGrid.SetAuthoritative(true);
        chestGrid.SetContainerId(1);
        chestGrid.Render();
    }

    // ─── Visual divider + Player inventory label ─────────────────────
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::Text("Player Inventory");
    ImGui::Separator();

    // Authoritative player grid (container_id=0) — fixes the null-inv_ deref
    // of the old inline SlotGridComponent (which never called SetInventory).
    ImGui::PushID("chest_player_inv");
    RenderPlayerInventoryGrid(*playerInv, 0, static_cast<int>(playerInv->slots.size()),
                              9, playerInv->selectedSlot, false, dragMgr_, /*authoritative*/ true);
    ImGui::PopID();

    // ─── Cursor preview (server-owned hand stack) ────────────────────
    if (playerInv->cursor.item_id != 0) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 mouse = ImGui::GetIO().MousePos;
        auto uv = renderlib::TextureAtlas::GetItemUV(playerInv->cursor.item_id);
        dl->AddImage(
            renderlib::TextureAtlas::GetTextureHandle().idx,
            ImVec2(mouse.x + 4, mouse.y + 4),
            ImVec2(mouse.x + 40 - 4, mouse.y + 40 - 4),
            ImVec2(uv.u0, uv.v0),
            ImVec2(uv.u1, uv.v1));
        if (playerInv->cursor.count > 1) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", playerInv->cursor.count);
            dl->AddText(ImVec2(mouse.x + 4, mouse.y + 4),
                        IM_COL32(255, 255, 255, 255), buf);
        }
    }

    ImGui::End();
}

bool ChestWindow::OnKeyEvent(int /*key*/, int /*action*/, int /*mods*/) {
    if (!open_) return false;
    return false;
}
