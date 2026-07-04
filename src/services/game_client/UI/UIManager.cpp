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
        dragMgr_.SetActionCallback([this](uint8_t actionType, uint8_t src, uint8_t tgt, uint8_t count) {
            if (!netClient_ || !playerInv_) return;
            if (dragMgr_.HasMachineDragContext() && src >= 100) {
                BlockPos pos = dragMgr_.GetMachineDragPos();
                int machineSlot = dragMgr_.GetMachineDragSlotIdx();
                uint8_t playerSlot = (actionType == DragManager::kActionDrop) ? 255 : tgt;
                netClient_->SendSetMachineSlot(playerInv_->player_id, pos,
                    static_cast<uint16_t>(machineSlot), 0, 0, 0, playerSlot);
                dragMgr_.ClearMachineDragContext();
                return;
            }
            netClient_->SendInventoryAction(playerInv_->player_id, actionType, src, tgt, count);
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
    binder_.Process(input, prevKeys_);
    std::memcpy(prevKeys_.data(), input.keys.data(), sizeof(prevKeys_));
}

void UIManager::RenderAll() {
    if (!playerInv_) return;

    // Clear hover tracking at start of each frame
    playerInv_->hoveredItemId = 0;

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
