#include "ClientCraftingWindow.h"
#include "Crafting/ClientItemRegistry.h"
#include "Components/SlotGrid.h"
#include "Components/PlayerInventoryGrid.h"
#include "Network/NetClient.h"
#include "core_generated.h"
#include <imgui.h>
#include <GLFW/glfw3.h>
#include <cstdio>
#include <spdlog/spdlog.h>
#include <flatbuffers/verifier.h>

CraftingWindow::CraftingWindow(BlockPos pos, NetClient* netClient, DragManager* dragMgr)
    : BlockAttachedWindow(pos)
    , dragMgr_(dragMgr)
    , netClient_(netClient)
{
    open_ = false;
}

void CraftingWindow::OnCraftResponse(bool success, uint16_t item_id, uint8_t count,
                                       uint16_t meta, const std::string& error,
                                       const std::array<ItemStack, 9>& grid) {
    if (success) {
        grid_.SetSlots(grid);
        grid_.SetResult(ItemStack{item_id, count, meta});
        craftToast_.lifetime = 0.0f;
        auto name = ItemRegistry::GetName(item_id);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Crafted %.*s x%d",
                      static_cast<int>(name.size()), name.data(), count);
        craftToast_.text = buf;
        craftToast_.color = ImVec4(0.3f, 1, 0.3f, 1);
        craftToast_.lifetime = 5.0f;
    } else {
        craftToast_.text = "⚠ " + error;
        craftToast_.color = ImVec4(1, 0.3f, 0.3f, 1);
        craftToast_.lifetime = 5.0f;
    }
}

bool CraftingWindow::OnKeyEvent(int key, int action, [[maybe_unused]] int mods) {
    if (!open_) return false;
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) { //TODO key use uidefaults
        SetOpen(false);
        return true;
    }
    return false;
}



void CraftingWindow::Render(InventoryState* playerInv) {
    if (!open_) return;

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::Begin("Workbench", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    craftToast_.Render();

    // Recalc вызывается StartDrag/EndDrag/CancelDrag — не нужно каждый кадр
    // (иначе затирает result_ после OnCraftResponse)

    constexpr float kSlotSize = 40.0f;
    SlotStyle gridStyle;
    gridStyle.size = static_cast<int>(kSlotSize);

    // ── 3×3 crafting grid + result panel ────────────────────────────────
    // Grid on the left, result slot + Craft button on the right.
    float gridStartX = ImGui::GetCursorPosX();
    float gridStartY = ImGui::GetCursorPosY();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float itemSpacingX = ImGui::GetStyle().ItemSpacing.x;
    const float itemSpacingY = ImGui::GetStyle().ItemSpacing.y;

    ImGui::PushID("craft_grid");
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            int idx = row * 3 + col;
            ImGui::PushID(idx);

            bool selected = false;
            bool activated = RenderSlot(grid_.Slots()[idx], selected, drawList, gridStyle);
            if (activated && dragMgr_) {
                grid_.HandleActivate(idx, *playerInv, *dragMgr_);
            }

            ImGui::PopID();
            if (col < 2) ImGui::SameLine();
        }
    }
    ImGui::PopID(); // craft_grid

    // ── Result slot (click to craft, Minecraft-style) ─────────────────
    float resultX = gridStartX + 3.0f * (kSlotSize + itemSpacingX) + 12.0f;
    ImGui::SetCursorPosX(resultX);
    ImGui::SetCursorPosY(gridStartY);

    ImGui::PushID("result");
    {
        ImVec2 slotPos = ImGui::GetCursorScreenPos();
        ImVec2 slotSize(kSlotSize, kSlotSize);
        drawList->AddRectFilled(slotPos,
                                ImVec2(slotPos.x + slotSize.x, slotPos.y + slotSize.y),
                                IM_COL32(255, 215, 0, 200));
        bool selected = false;
        bool activated = RenderSlot(grid_.GetResult(), selected, drawList, gridStyle);

        if (activated && grid_.GetResult().item_id != 0) {
            if (netClient_) {
                netClient_->SendCraftRequest(playerInv->player_id, GetAnchorPos(), grid_.Slots().data());
            }
        }
    }
    ImGui::PopID(); // result

    // Restore cursor below the grid for subsequent content
    float gridEndY = gridStartY + 3.0f * (kSlotSize + itemSpacingY);
    ImGui::SetCursorPosY(gridEndY);
    ImGui::SetCursorPosX(gridStartX);

    if (dragMgr_ && dragMgr_->IsDragging()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const auto& held = dragMgr_->GetHeldItem();
        uint32_t itemColor = IM_COL32(
            held.item_id * 50 % 256,
            held.item_id * 80 % 256,
            held.item_id * 30 % 256, 255);
        ImVec2 mousePos = ImGui::GetIO().MousePos;
        dl->AddRectFilled(ImVec2(mousePos.x + 4, mousePos.y + 4),
                          ImVec2(mousePos.x + kSlotSize - 4, mousePos.y + kSlotSize - 4),
                          itemColor, 2.0f);
        if (held.count > 1) {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%d", held.count);
            dl->AddText(ImVec2(mousePos.x + 4, mousePos.y + 4),
                        IM_COL32(255, 255, 255, 255), buf);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            grid_.HandleCancel(*dragMgr_);
        }
    }

    ImGui::Separator();

    ImGui::PushID("player_inv");
    RenderPlayerInventoryGrid(*playerInv, 0, static_cast<int>(playerInv->slots.size()), 9, -1, false, dragMgr_);
    ImGui::PopID();

    ImGui::End();
}

void CraftingWindow::OnNetworkUpdate(uint8_t msgType, const void* data) {
    if (msgType != GatewayMsg::kCraftResponse) {
        return;
    }

    if (!data) {
        return;
    }

    // ── Parse FlatBuffer CraftResponse ────────────────────────────
    flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(data), 8192);
    if (!v.VerifyBuffer<Protocol::CraftResponse>(nullptr)) {
        spdlog::warn("CraftingWindow: invalid CraftResponse buffer");
        return;
    }

    auto* resp = flatbuffers::GetRoot<Protocol::CraftResponse>(data);

    // Extract result ItemStack
    auto* r = resp->result();
    std::array<ItemStack, 9> grid{};
    if (auto* fbGrid = resp->grid()) {
        for (uint16_t i = 0; i < 9 && i < fbGrid->size(); ++i) {
            auto* gs = fbGrid->Get(i);
            if (gs) {
                grid[i] = ItemStack{
                    static_cast<uint16_t>(gs->item_id()),
                    static_cast<uint8_t>(gs->count()),
                    static_cast<uint16_t>(gs->meta())};
            }
        }
    }

    // Call OnCraftResponse with parsed data
    OnCraftResponse(
        resp->success(),
        r ? static_cast<uint16_t>(r->item_id()) : 0,
        r ? static_cast<uint8_t>(r->count()) : 0,
        r ? static_cast<uint16_t>(r->meta()) : 0,
        resp->error() ? resp->error()->str() : "",
        grid
    );
}
