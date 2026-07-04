#include "PlayerInventory.h"
#include "Network/NetClient.h"
#include "core_generated.h"
#include "gateway_generated.h"
#include <imgui.h>
#include <spdlog/spdlog.h>

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
        return;
    }
    IUIWindow::OnNetworkUpdate(msgType, data);
}

// ── Render ──────────────────────────────────────────────────────────────────
void PlayerInventory::Render(InventoryState* /*playerInv*/) {
    if (++g_invFrame % 60 == 0) {
        //spdlog::info("PlayerInventory::Render open={} drag={}", state_.open, state_.isDragging);
    }
    RenderHotbar(state_.slots, state_.selectedSlot);

    // Manual hotbar hover tracking (works outside ImGui windows)
    {
        SlotStyle s;
        constexpr int kHS = 10;
        float totalW = kHS * static_cast<float>(s.size + s.padding);
        float startX = (ImGui::GetIO().DisplaySize.x - totalW) / 2.0f;
        float y = ImGui::GetIO().DisplaySize.y - s.size - 20.0f;
        ImVec2 mouse = ImGui::GetIO().MousePos;
        if (mouse.y >= y && mouse.y <= y + static_cast<float>(s.size)) {
            int slot = static_cast<int>((mouse.x - startX) / static_cast<float>(s.size + s.padding));
            if (slot >= 0 && slot < kHS && static_cast<size_t>(slot) < state_.slots.size()) {
                state_.hoveredItemId = state_.slots[slot].item_id;
            }
        }
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
    int clicked = RenderPlayerInventoryGrid(state_, kHotbarSlots, kTotalSlots - kHotbarSlots, kInventoryCols, state_.selectedSlot, true, dragMgr_);
    if (clicked >= 0) {
        if (!dragMgr_->IsDragging()) state_.selectedSlot = clicked;
        spdlog::info("PlayerInv: clicked slot={} dragging={}", clicked, dragMgr_->IsDragging());
    }

  ImGui::Separator();

  // ── Hotbar row ────────────────────────────────────────────────────────
  int hotbarClicked = RenderPlayerInventoryGrid(state_, 0, kHotbarSlots, kInventoryCols, state_.selectedSlot, true, dragMgr_);
  if (hotbarClicked >= 0) {
      if (!dragMgr_->IsDragging()) state_.selectedSlot = hotbarClicked;
      spdlog::info("PlayerInv(hotbar): clicked slot={} dragging={}", hotbarClicked, dragMgr_->IsDragging());
  }
  ImGui::End();
}