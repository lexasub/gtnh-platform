#include "PlayerInventory.h"
#include "Network/NetClient.h"
#include "RenderLib/Utils/TextureAtlas.h"
#include "core_generated.h"
#include "gateway_generated.h"
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <cstdio>

static int g_invFrame = 0;

static constexpr int kHotbarSlots = 10;
static constexpr int kInventoryRows = 4;
static constexpr int kInventoryCols = 10;
static constexpr int kTotalSlots = kInventoryRows * kInventoryCols; // 40

PlayerInventory::PlayerInventory(InventoryState& state) : state_(state) {}

// ── Key event ───────────────────────────────────────────────────────────────
// E (INVENTORY) is handled by InputBinder → ActionHandler::DoToggleInventory.
bool PlayerInventory::OnKeyEvent(int /*key*/, int /*action*/, int /*mods*/) {
    return false;
}

void PlayerInventory::OnNetworkUpdate(uint8_t msgType, const void *data) {
    if (msgType == GatewayMsg::kInventoryUpdate) {
        flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(data), 2048);
        if (!v.VerifyBuffer<Protocol::InventoryUpdate>(nullptr)) {
            spdlog::warn("PlayerInventory: invalid InventoryUpdate");
            return;
        }
        auto* update = flatbuffers::GetRoot<Protocol::InventoryUpdate>(data);
        if (update->player_id() != state_.player_id) return;
        auto* slots = update->slots();
        if (!slots) return;
        // Clear all slots first, then fill from server snapshot (positional)
        for (auto& slot : state_.slots) slot = {0, 0, 0};
        size_t n = std::min(static_cast<size_t>(slots->size()), state_.slots.size());
        for (size_t i = 0; i < n; ++i) {
            auto* s = slots->Get(i);
            if (s && s->item_id() != 0) {
                state_.slots[i] = {s->item_id(), s->count(), s->meta()};
            }
        }
        // Server-owned cursor stack (authoritative click model).
        state_.cursor = ItemStack{0, 0, 0};
        if (auto* cur = update->cursor()) {
            state_.cursor = {cur->item_id(), cur->count(), cur->meta()};
        }
        return;
    }
    IUIWindow::OnNetworkUpdate(msgType, data);
}

// ── Render ──────────────────────────────────────────────────────────────────
void PlayerInventory::Render(InventoryState* /*playerInv*/) {
    if (++g_invFrame % 60 == 0) {
        //spdlog::info("PlayerInventory::Render open={} drag={}", state_.open, state_.isDragging);
    }
    int hotbarHover = RenderHotbar(state_.slots, state_.selectedSlot, SlotStyle{}, dragMgr_);

    if (hotbarHover >= 0 && static_cast<size_t>(hotbarHover) < state_.slots.size()) {
        state_.hoveredItemId = state_.slots[hotbarHover].item_id;
    }

    if (!state_.open && hotbarHover >= 0 && dragMgr_ &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
         ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        int button = ImGui::IsMouseClicked(ImGuiMouseButton_Right) ? 1 : 0;
        bool shift = ImGui::GetIO().KeyShift;
        bool ctrl = ImGui::GetIO().KeyCtrl;
        // Authoritative click path — the server owns the cursor.
        dragMgr_->OnPlayerSlotClick(hotbarHover, button, shift, ctrl);
    }

    if (!state_.open) return;

    ImGui::SetNextWindowBgAlpha(0.8f);
    ImGui::SetNextWindowPos(
        ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("Inventory", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

    // ── Inventory grid (rows 1-4, skipping hotbar) ────────────────────────
    int clicked = RenderPlayerInventoryGrid(state_, kHotbarSlots, kTotalSlots - kHotbarSlots, kInventoryCols, state_.selectedSlot, true, dragMgr_, true);
    if (clicked >= 0) {
        if (!dragMgr_->IsDragging()) state_.selectedSlot = clicked;
        spdlog::info("PlayerInv: clicked slot={} dragging={}", clicked, dragMgr_->IsDragging());
    }

  ImGui::Separator();

  // ── Hotbar row ────────────────────────────────────────────────────────
  int hotbarClicked = RenderPlayerInventoryGrid(state_, 0, kHotbarSlots, kInventoryCols, state_.selectedSlot, true, dragMgr_, true);
  if (hotbarClicked >= 0) {
      if (!dragMgr_->IsDragging()) state_.selectedSlot = hotbarClicked;
      spdlog::info("PlayerInv(hotbar): clicked slot={} dragging={}", hotbarClicked, dragMgr_->IsDragging());
  }

  // ── Cursor preview (server-owned hand stack) ──────────────────────────
  if (state_.cursor.item_id != 0) {
      ImDrawList* dl = ImGui::GetForegroundDrawList();
      ImVec2 mouse = ImGui::GetIO().MousePos;
      auto uv = renderlib::TextureAtlas::GetItemUV(state_.cursor.item_id);
      dl->AddImage(
          renderlib::TextureAtlas::GetTextureHandle().idx,
          ImVec2(mouse.x + 4, mouse.y + 4),
          ImVec2(mouse.x + 40 - 4, mouse.y + 40 - 4),
          ImVec2(uv.u0, uv.v0),
          ImVec2(uv.u1, uv.v1));
      if (state_.cursor.count > 1) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "%d", state_.cursor.count);
          dl->AddText(ImVec2(mouse.x + 4, mouse.y + 4),
                      IM_COL32(255, 255, 255, 255), buf);
      }
  }

  ImGui::End();
}