#include "ClientCraftingWindow.h"
#include "Crafting/ClientItemRegistry.h"
#include "Components/SlotGrid.h"
#include "Components/PlayerInventoryGrid.h"
#include "Network/NetClient.h"
#include "RenderLib/Utils/TextureAtlas.h"
#include "core_generated.h"
#include <common/ItemId.h>
#include <imgui.h>
#include <cstdio>
#include <spdlog/spdlog.h>
#include <flatbuffers/verifier.h>

namespace {
constexpr uint16_t kCraftingTableId = ItemId::pack("0:10:11:1");
} // namespace

CraftingWindow::CraftingWindow(BlockPos pos, NetClient* netClient, DragManager* dragMgr,
                               ServerRecipeDB* recipeDb)
    : BlockAttachedWindow(pos)
    , gridSlots_(9)
    , gridComp_(gridSlots_)
    , dragMgr_(dragMgr)
    , netClient_(netClient)
    , recipeDb_(recipeDb)
{
    open_ = false;
    // Authoritative container: the server owns the grid; every click is an
    // InventoryAction with container_id=1, snapshots come back via
    // kInventoryUpdate. Local mutation is disabled (CraftingGrid is a plain
    // mirror for the preview only).
    gridComp_.SetRange(0, 9, 3);
    gridComp_.SetDragManager(dragMgr_);
    gridComp_.SetAuthoritative(true);
    gridComp_.SetContainerId(1);
    // Server-driven live preview: the client has no recipe knowledge of its
    // own — every grid change is checked against the server's recipe table
    // (results cached by ServerRecipeDB, LRU on grid hash).
    grid_.onGridChanged_ = [this](const std::array<ItemStack, 9>& g) {
        if (!recipeDb_) return;
        uint32_t gen = grid_.Generation();
        recipeDb_->CheckGrid(kCraftingTableId, g, [this, gen](const ItemStack& out) {
            grid_.ApplyServerResult(gen, out);
        });
    };
}

void CraftingWindow::SetOpen(bool open) {
    if (open && !open_) {
        // Request a container session + saved grid state from the server.
        if (netClient_ && player_id_ != 0) {
            netClient_->SendWorkbenchOpenReq(player_id_, GetAnchorPos());
        }
    }
    open_ = open;
}

void CraftingWindow::OnCraftResponse(bool success, uint16_t item_id, uint8_t count,
                                       uint16_t meta, const std::string& error,
                                       const std::array<ItemStack, 9>& grid) {
    if (success) {
        grid_.SetSlots(grid);
        gridSlots_.assign(grid.begin(), grid.end());
        grid_.SetResult(ItemStack{item_id, count, meta});
        // Any in-flight grid-check replies for the (now consumed) grid are
        // stale — drop them so they don't erase the "crafted" result slot.
        grid_.InvalidatePreview();
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

bool CraftingWindow::OnKeyEvent(int /*key*/, int /*action*/, int /*mods*/) {
    if (!open_) return false;
    // ESCAPE close is handled by InputBinder → close_ui → UIManager::TopWindow
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
    // Grid on the left, result slot on the right. The grid is a
    // server-authoritative container (SlotGridComponent, container_id=1) —
    // clicks go to the server, snapshots arrive via kInventoryUpdate.
    float gridStartX = ImGui::GetCursorPosX();
    float gridStartY = ImGui::GetCursorPosY();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float itemSpacingX = ImGui::GetStyle().ItemSpacing.x;
    const float itemSpacingY = ImGui::GetStyle().ItemSpacing.y;

    ImGui::PushID("craft_grid");
    gridComp_.SetStyle(gridStyle);
    gridComp_.SetInventory(*playerInv);
    gridComp_.Render();
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

    // Cursor preview (server-owned hand stack). The legacy drag preview
    // (dragMgr_->GetHeldItem()) is always empty in authoritative mode — the
    // hand stack lives in playerInv->cursor, published by the server. Without
    // this, picked-up items were invisible in the workbench ("item doesn't
    // fly with the cursor").
    if (playerInv->cursor.item_id != 0) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 mouse = ImGui::GetIO().MousePos;
        auto uv = renderlib::TextureAtlas::GetItemUV(playerInv->cursor.item_id);
        dl->AddImage(
            renderlib::TextureAtlas::GetTextureHandle().idx,
            ImVec2(mouse.x + 4, mouse.y + 4),
            ImVec2(mouse.x + kSlotSize - 4, mouse.y + kSlotSize - 4),
            ImVec2(uv.u0, uv.v0),
            ImVec2(uv.u1, uv.v1));
        if (playerInv->cursor.count > 1) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", playerInv->cursor.count);
            dl->AddText(ImVec2(mouse.x + 4, mouse.y + 4),
                        IM_COL32(255, 255, 255, 255), buf);
        }
    }

    ImGui::Separator();

    ImGui::PushID("player_inv");
    RenderPlayerInventoryGrid(*playerInv, 0, static_cast<int>(playerInv->slots.size()), 9, -1, false, dragMgr_, /*authoritative*/ true);
    ImGui::PopID();

    ImGui::End();
}

void CraftingWindow::OnNetworkUpdate(uint8_t msgType, const void* data) {
    if (msgType == GatewayMsg::kInventoryUpdate) {
        // Authoritative container snapshot (container_id=1) for this workbench.
        if (!data) return;
        flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(data), 8192);
        if (!v.VerifyBuffer<Protocol::InventoryUpdate>(nullptr)) return;
        auto* update = flatbuffers::GetRoot<Protocol::InventoryUpdate>(data);
        if (update->container_id() != 1) return;
        auto* cp = update->container_pos();
        if (!cp || cp->x() != GetAnchorPos().x || cp->y() != GetAnchorPos().y ||
            cp->z() != GetAnchorPos().z)
            return;
        auto* cs = update->container_slots();
        std::array<ItemStack, 9> grid{};
        size_t n = std::min(static_cast<size_t>(cs ? cs->size() : 0), grid.size());
        for (size_t i = 0; i < n; ++i) {
            auto* s = cs->Get(i);
            if (s) {
                grid[i] = ItemStack{
                    static_cast<uint16_t>(s->item_id()),
                    static_cast<uint8_t>(s->count()),
                    static_cast<uint16_t>(s->meta())};
            }
        }
        grid_.SetSlots(grid);
        grid_.Recalc(); // server snapshot → refresh preview
        // The visible grid (gridComp_) renders gridSlots_, not grid_. Without
        // syncing here the slots stayed empty after any server mutation, so
        // placed items seemed to vanish ("click and it doesn't place").
        gridSlots_.assign(grid.begin(), grid.end());
        spdlog::debug("[CraftingWindow] container snapshot applied at ({},{},{})",
                      GetAnchorPos().x, GetAnchorPos().y, GetAnchorPos().z);
        return;
    }

    if (msgType == GatewayMsg::kGridUpdate) {
        if (!data) return;
        flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(data), 4096);
        if (!v.VerifyBuffer<Protocol::GridUpdate>(nullptr)) {
            spdlog::warn("CraftingWindow: invalid GridUpdate buffer");
            return;
        }
        auto* gu = flatbuffers::GetRoot<Protocol::GridUpdate>(data);
        if (!gu || !gu->pos()) return;
        // Only apply if position matches this window's workbench.
        auto p = gu->pos();
        if (p->x() != GetAnchorPos().x || p->y() != GetAnchorPos().y ||
            p->z() != GetAnchorPos().z)
            return;
        std::array<ItemStack, 9> grid{};
        if (gu->grid()) {
            for (uint16_t i = 0; i < 9 && i < gu->grid()->size(); ++i) {
                auto* gs = gu->grid()->Get(i);
                if (gs) {
                    grid[i] = ItemStack{
                        static_cast<uint16_t>(gs->item_id()),
                        static_cast<uint8_t>(gs->count()),
                        static_cast<uint16_t>(gs->meta())};
                }
            }
        }
        grid_.SetSlots(grid);
        gridSlots_.assign(grid.begin(), grid.end());
        spdlog::debug("[CraftingWindow] GridUpdate applied at ({},{},{})",
                      p->x(), p->y(), p->z());
        return;
    }

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
