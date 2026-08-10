#include "UIManager.h"
#include "Windows/BlockAttachedWindow.h"
#include "Windows/player/PlayerInventory.h"
#include "Windows/block/MachineWindow.h"
#include "Windows/player/RecipeInspectWindow.h"
#include "Panels/NeiPanel.h"
#include "Common/Types.h"
#include "Network/NetClient.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <cstring>

#include "Inventory.h"

void UIManager::SetNetClient(NetClient* nc) {
    netClient_ = nc;
    if (netClient_ && playerInv_) {
        // ── Legacy mutation callback (container grids) ─────────────────────
        // Machine slots are now authoritative grids (container_id=1, Phase C):
        // clicks go through SetClickCallback below, and SetMachineSlotReq is
        // retired. Craft-grid and chest slot sources are client-side staging
        // synced via their own protocols (craft request, chest close-save) —
        // never a legacy InventoryAction; player-inventory moves go through
        // the authoritative click callback below.
        dragMgr_.SetActionCallback([this](uint8_t /*actionType*/, uint8_t src, uint8_t /*tgt*/, uint8_t /*count*/) {
            if (!netClient_ || !playerInv_) return;
            if (src >= DragManager::kGridSlotBase) return;
            if (src >= DragManager::kChestSlotBase) return;
        });

        // ── Authoritative click path (server owns the cursor) ──────────────
        dragMgr_.SetClickCallback([this](const DragManager::ClickInfo& info) {
            if (!netClient_ || !playerInv_) return;
            netClient_->SendInventoryAction(playerInv_->player_id, info.actionType,
                                            info.button, info.mods, info.containerId,
                                            info.slot, info.count);
        });
    }
}

void UIManager::RenderPanels() {
    for (auto& p : panels_) {
        if (p->IsVisible()) {
            p->Render(playerInv_);
        }
    }
}

void UIManager::ProcessInput(const InputState& input) {
    // While ImGui owns an active text input (console, NEI/creative/quest
    // search), suppress global hotkeys so typing doesn't trigger game
    // actions (E=inventory, Tab=creative, U=item list, 1-9=hotbar, ...).
    // ESC (close_ui) and F4 (toggle_console) stay live.
    binder_.SetTextCapture(ImGui::GetIO().WantTextInput);
    binder_.Process(input, prevKeys_);
    std::memcpy(prevKeys_.data(), input.keys.data(), sizeof(prevKeys_));
}

void UIManager::RenderAll() {
    if (!playerInv_) return;

    // Clear hover tracking at start of each frame (single reset point — grids
    // only write hover while the mouse is over their slots)
    playerInv_->hoveredItemId = 0;
    playerInv_->dragHoverSlot = -1;
    playerInv_->hoveredSlot = -1;
    dragMgr_.UpdateHover(-1);

    // Sync DragManager → InventoryState before FIRST window render.
    // Also sync before EACH window below, because DragManager state can
    // change during SlotGridComponent::Render (item pickup).
    auto sync = [this]() { dragMgr_.SyncTo(*playerInv_); };

    sync();
    if (auto* inv = FindByType<PlayerInventory>()) {
        sync();
        inv->Render(playerInv_);
    }
    for (auto& w : windows_) {
        if (w->IsOpen() && w.get() != FindByType<PlayerInventory>() && playerInv_) {
            sync();
            w->Render(playerInv_);
        }
    }

    RenderPanels();
}

void UIManager::HandleNetwork(uint8_t msgType, const void* data) {
    for (auto& w : windows_) {
        w->OnNetworkUpdate(msgType, data);
    }
}

void UIManager::CloseAll() {
    for (auto& w : windows_) {
        w->SetOpen(false);
    }
    if (playerInv_) {
        playerInv_->open = false;
    }
}

bool UIManager::AnyOpen() const {
    for (auto& w : windows_) {
        if (w->IsOpen()) return true;
    }
    return playerInv_ && playerInv_->open;
}

IUIWindow* UIManager::TopWindow() const {
    for (auto it = windows_.rbegin(); it != windows_.rend(); ++it) {
        if ((*it)->IsOpen()) return it->get();
    }
    return nullptr;
}

void UIManager::OpenExclusive(IUIWindow* window) {
    if (!window) return;

    if (window->IsOpen()) {
        window->SetOpen(false);
        return;
    }

    for (auto& w : windows_) {
        if (w.get() != window) {
            w->SetOpen(false);
        }
    }
    if (playerInv_) {
        playerInv_->open = false;
    }

    window->SetOpen(true);
}

IUIWindow* UIManager::FindOpenAtBlock(const BlockPos& pos) const {
    for (auto& w : windows_) {
        if (w->IsOpen() && w->IsBlockAttached()) {
            auto* ba = static_cast<BlockAttachedWindow*>(w.get());
            if (ba->GetAnchorPos() == pos) {
                return w.get();
            }
        }
    }
    return nullptr;
}
