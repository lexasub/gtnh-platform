#include "ChestWindow.h"
#include "Common/Inventory.h"
#include "Components/SlotGrid.h"
#include "UI/Core/DragManager.h"
#include "core_generated.h"
#include <imgui.h>
#include <GLFW/glfw3.h>
#include <cstdio>

ChestWindow::ChestWindow(BlockPos pos)
    : BlockAttachedWindow(pos)
    , open_(false)
    , chestSlots_(27) {}

void ChestWindow::SetOpen(bool open) {
    open_ = open;
    if (open_) sendOpenReq();
    else sendCloseReq();
}

void ChestWindow::sendOpenReq() {
    if (netClient_) netClient_->SendChestOpen(0, pos_);
}

void ChestWindow::sendCloseReq() {
    // ChestOpenReq schema has no open/close flag — server always returns inventory.
    if (netClient_) netClient_->SendChestOpen(0, pos_);
}

void ChestWindow::onChestSlotAck(BlockPos pos, bool success, const std::vector<ItemStack>& slots) {
    if (!success || pos != pos_) return;
    chestSlots_ = slots;
}

void ChestWindow::OnNetworkUpdate(uint8_t msgType, const void* data) {
    if (msgType == GatewayMsg::kChestOpenResp) {
        flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(data), 8192);
        if (!v.VerifyBuffer<Protocol::ChestOpenResp>(nullptr)) return;
        auto resp = flatbuffers::GetRoot<Protocol::ChestOpenResp>(data);
        if (!resp || !resp->pos()) return;
        if (resp->pos()->x() != pos_.x || resp->pos()->y() != pos_.y || resp->pos()->z() != pos_.z) return;
        auto* fbSlots = resp->slots();
        if (!fbSlots) return;
        chestSlots_.clear();
        chestSlots_.reserve(fbSlots->size());
        for (flatbuffers::uoffset_t i = 0; i < fbSlots->size(); ++i) {
            auto* s = fbSlots->Get(i);
            if (s) chestSlots_.push_back({static_cast<uint16_t>(s->item_id()), s->count(), static_cast<uint16_t>(s->meta())});
        }
    }
}

void ChestWindow::Render(InventoryState* playerInv) {
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
        chestGrid.SetSelectedSlot(-1);
        chestGrid.SetInventory(*playerInv);
        if (dragMgr_) chestGrid.SetDragManager(dragMgr_);
        chestGrid.Render();
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

        SlotGridComponent playerGrid(playerInv->slots);
        playerGrid.SetStyle(style);
        playerGrid.SetRange(0, static_cast<int>(playerInv->slots.size()), 9);
        playerGrid.SetSelectedSlot(-1);
        playerGrid.SetInventory(*playerInv);
        if (dragMgr_) playerGrid.SetDragManager(dragMgr_);
        playerGrid.Render();
    }
    ImGui::PopID();

    ImGui::End();
}

bool ChestWindow::OnKeyEvent(int key, int action, [[maybe_unused]] int mods) {
    if (!open_) return false;
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) { //TODO key via uidefaults
        SetOpen(false);
        return true;
    }
    return false;
}
