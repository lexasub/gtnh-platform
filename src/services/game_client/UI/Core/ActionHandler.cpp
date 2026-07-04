#include "Core/ActionHandler.h"
#include "Core/ActionRegistry.h"
#include "UIManager.h"
#include "Windows/player/PlayerInventory.h"
#include "Windows/player/CreativeMenu.h"
#include "../Windows/player/RecipeInspectWindow.h"
#include "../Windows/player/QuestBookWindow.h"
#include "Panels/NeiPanel.h"
#include "Network/NetClient.h"
#include "Common/Inventory.h"

#include "gateway_generated.h"
#include "core_generated.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

void ActionHandler::Init(ActionRegistry* reg, UIManager* mgr, NetClient* nc,
                         InventoryState* inv) {
    reg_ = reg;
    uiMgr_ = mgr;
    netClient_ = nc;
    playerInv_ = inv;

    // Wire InputBinder to ActionRegistry so key presses can find actions
    if (uiMgr_) {
        uiMgr_->GetBinder().SetActionRegistry(reg);
    }

    // Register all built-in actions
    reg->Register("toggle_item_list",  [this]() { DoToggleItemList(); });//TODO give object func instead lambda
    reg->Register("show_recipe",       [this]() { DoShowRecipeForHovered(); });
    reg->Register("close_ui",          [this]() { DoCloseAll(); });
    reg->Register("toggle_inv",        [this]() { DoToggleInventory(); });
    reg->Register("toggle_creative",   [this]() { DoToggleCreativeMenu(); });
    reg->Register("toggle_quest_book", [this]() { DoToggleQuestBook(); });
    reg->Register("INVENTORY",         [this]() { DoToggleInventory(); });
    reg->Register("CREATIVE_MENU",     [this]() { DoToggleCreativeMenu(); });
    for (int i = 0; i < 10; ++i) {
        auto name = "hotbar_" + std::to_string(i);
        reg->Register(name, [this, i]() { DoSelectHotbar(i); });
    }
    // Scroll binding (not a named action — wired as InputBinder callback)
    if (uiMgr_) {
        uiMgr_->GetBinder().OnScroll([this](float delta) { DoScrollHotbar(delta); });//TODO give object func instead lambda
    }
}

void ActionHandler::DoToggleItemList() {
    if (auto* rp = uiMgr_->FindPanel<NeiPanel>()) {
        rp->SetVisible(!rp->IsVisible());
    }
}

void ActionHandler::DoShowRecipeForHovered() {
    if (ImGui::GetIO().WantTextInput || playerInv_ == nullptr) return;
    if (playerInv_->hoveredItemId != 0 && !playerInv_->isDragging) {
        DoOpenRecipeInspect(playerInv_->hoveredItemId);
    }
}

void ActionHandler::DoCloseAll() {
    uiMgr_->CloseAll();
}

void ActionHandler::DoToggleInventory() {
    if (auto* inv = uiMgr_->FindByType<PlayerInventory>()) {
        inv->SetOpen(!inv->IsOpen());
    }
}

void ActionHandler::DoToggleCreativeMenu() {
    if (auto* menu = uiMgr_->FindByType<CreativeMenu>()) {
        menu->SetOpen(!menu->IsOpen());
    }
}

void ActionHandler::DoToggleQuestBook() {
    if (auto* qb = uiMgr_->FindByType<QuestBookWindow>()) {
        qb->SetOpen(!qb->IsOpen());
        spdlog::debug("[Quest] Toggle quest book window: open={}", qb->IsOpen());
    }
}

void ActionHandler::DoSelectHotbar(int slot) {
    if (playerInv_) playerInv_->selectedSlot = slot;
}

void ActionHandler::DoScrollHotbar(float delta) {
    if (playerInv_) {
        playerInv_->selectedSlot =
            (playerInv_->selectedSlot + (delta > 0.0f ? -1 : 1) + 10) % 10;
    }
}

void ActionHandler::DoOpenRecipeInspect(uint16_t itemId) {
    if (auto* w = uiMgr_->FindByType<RecipeInspectWindow>()) {
        w->SetItem(itemId);
        w->SetOpen(!w->IsOpen());
    }
}

void ActionHandler::SpawnItem(uint16_t itemId, uint8_t count, int16_t targetSlot) {
    if (!netClient_ || !playerInv_) return;
    netClient_->SendPlayerAction(
        playerInv_->player_id,
        Protocol::PlayerActionType::PlayerActionType_ITEM_ACTION,
        targetSlot, 0, 0,
        itemId, count);
}
