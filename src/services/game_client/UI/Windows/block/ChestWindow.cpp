#include "ChestWindow.h"
#include "Common/Inventory.h"
#include "Components/SlotGrid.h"
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
        // Slots are unloaded until BlockEntityUpdate arrives — saving before that wipes server state.
        chestSlots_.assign(27, ItemStack{});
        dataLoaded_ = false;
    }
    if (!open && open_ && netClient_ && lastPlayerInv_) {
        if (dataLoaded_) {
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

void ChestWindow::Render(InventoryState* playerInv) {
    lastPlayerInv_ = playerInv;
    if (!open_) return;

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

        SlotGridComponent chestGrid(chestSlots_);
        chestGrid.SetStyle(style);
        chestGrid.SetRange(0, static_cast<int>(chestSlots_.size()), 9);
        chestGrid.SetSlotIndexOffset(DragManager::kChestSlotBase);
        chestGrid.SetDragManager(dragMgr_);
        chestGrid.SetInventory(*playerInv);
        chestGrid.Render();
    }

    // ─── Visual divider + Player inventory label ─────────────────────
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::Text("Player Inventory");
    ImGui::Separator();

    {
        SlotStyle style;
        style.size = 40;
        style.padding = 2;
        style.showNumbers = true;
        style.drawBackground = true;

        SlotGridComponent invGrid(playerInv->slots);
        invGrid.SetStyle(style);
        invGrid.SetRange(0, static_cast<int>(playerInv->slots.size()), 9);
        invGrid.SetDragManager(dragMgr_);
        invGrid.Render();
    }

    // ─── Drag preview (rendered by DragManager) ──────────────────────
    if (dragMgr_ && dragMgr_->IsDragging()) {
        SlotStyle style;
        dragMgr_->RenderPreview(style);
    }

    ImGui::End();
}

bool ChestWindow::OnKeyEvent(int /*key*/, int /*action*/, int /*mods*/) {
    if (!open_) return false;
    return false;
}
