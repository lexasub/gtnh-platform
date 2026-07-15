#include "Panels/NeiPanel.h"
#include "Crafting/ClientMachineRecipeDB.h"
#include "Crafting/ClientRecipeDB.h"
#include "Crafting/ClientItemRegistry.h"
#include "UI/Components/ItemIndex.h"
#include "UIManager.h"
#include "../Windows/block/MachineWindow.h"
#include "Components/SlotGrid.h"
#include "Core/ActionHandler.h"
#include "Common/Types.h"
#include "RenderLib/Utils/TextureAtlas.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <cstdio>
#include <algorithm>
#include <unordered_set>

NeiPanel::NeiPanel(UIManager* uiMgr) : uiMgr_(uiMgr) {}

bool NeiPanel::OnKeyEvent(int key, int action, int /*mods*/) {
    if (!visible_) return false;
    if (action == GLFW_PRESS && key == GLFW_KEY_ESCAPE) { //TODO use key via uidefaults
        visible_ = false;
        return true;
    }
    return false;
}

void NeiPanel::Render([[maybe_unused]] InventoryState* playerInv) {
    if (!visible_) return;

    auto* mw = uiMgr_ ? uiMgr_->FindByType<MachineWindow>() : nullptr;
    if (mw && mw->IsOpen()) {
        activeMachinePos_ = mw->GetAnchorPos();
        RenderMachineRecipes(mw);
    } else {
        RenderAllRecipes();
    }
}

void NeiPanel::RenderMachineRecipes(MachineWindow* mw) {
    uint16_t machineType = mw->GetMachineType();
    const auto& recipes = MachineRecipes::GetRecipes(machineType);

    auto title = std::string("Recipes for Machine ") + std::to_string(machineType);
    ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("Machine %u Recipes", machineType);
    ImGui::Separator();

    if (recipes.empty()) {
        ImGui::Text("No recipes found");
        ImGui::End();
        return;
    }

    if (selectedRecipe_ >= static_cast<int>(recipes.size()))
        selectedRecipe_ = -1;

    ImGui::BeginChild("recipeList", ImVec2(220, 300), true);
    for (int i = 0; i < static_cast<int>(recipes.size()); ++i) {
        ImGui::PushID(i);
        bool selected = (i == selectedRecipe_);
        if (ImGui::Selectable(recipes[i].name.c_str(), &selected))
            selectedRecipe_ = i;
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("recipeDetail", ImVec2(300, 300), true);
    if (selectedRecipe_ >= 0) {
        const auto& recipe = recipes[selectedRecipe_];
        ImGui::Text("%s", recipe.name.c_str());
        ImGui::Separator();

        auto spawnItem = [this](const ItemStack& item) {
            if (uiMgr_ && item.item_id != 0) {
                auto* inv = uiMgr_->GetPlayerInventory();
                int16_t ts = inv ? static_cast<int16_t>(inv->selectedSlot) : -1;
                uiMgr_->GetActions().SpawnItem(item.item_id, item.count, ts);
            }
        };

        int slotIdx = 0;
        for (const auto& input : recipe.inputs) {
            ImGui::PushID(slotIdx++);
            if (RenderSlot(input, false, ImGui::GetWindowDrawList())) {
                spawnItem(input);
            }
            ImGui::PopID();
            ImGui::SameLine();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", ItemRegistry::GetName(input.item_id).data());
        }

        if (!recipe.inputs.empty()) {
            ImGui::SameLine();
            ImGui::Text(" \xE2\x86\x92 ");
            ImGui::SameLine();
        }

        for (const auto& output : recipe.outputs) {
            ImGui::PushID(slotIdx++);
            if (RenderSlot(output, false, ImGui::GetWindowDrawList())) {
                spawnItem(output);
            }
            ImGui::PopID();
            ImGui::SameLine();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", ItemRegistry::GetName(output.item_id).data());
        }

        ImGui::NewLine();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Duration: %u ticks", recipe.duration);
        ImGui::Text("%s", buf);
    }
    ImGui::EndChild();

    ImGui::End();
}

void NeiPanel::RenderAllRecipes() {
    const auto& io = ImGui::GetIO();
    float panelW = 380.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - panelW, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, io.DisplaySize.y), ImGuiCond_Always);
    ImGui::Begin("NEI Items", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (justOpened_) {
        itemIndex_.Rebuild();
        ImGui::SetKeyboardFocusHere();
        justOpened_ = false;
    }
    ImGui::InputTextWithHint("##search", "Search items...", searchBuf_, sizeof(searchBuf_));

    itemIndex_.SetSearch(searchBuf_);
    const auto& filteredItems = itemIndex_.GetItems();

    // ── 8-column item grid with names ────────────────────────────────────
    constexpr float kIconSize = 32.0f;
    constexpr int kCols = 8;
    float cellW = (panelW - 8.0f) / kCols;
    float nameH = ImGui::GetTextLineHeight();

    ImGui::BeginChild("itemGrid", ImVec2(0, 0), true);

    auto spawnTargetSlot = [&]() -> int16_t {
        auto* inv = uiMgr_ ? uiMgr_->GetPlayerInventory() : nullptr;
        return inv ? static_cast<int16_t>(inv->selectedSlot) : -1;
    };

    for (int idx = 0; idx < static_cast<int>(filteredItems.size()); ++idx) {
        int col = idx % kCols;
        if (col > 0) ImGui::SameLine();

        uint16_t itemId = filteredItems[idx];
        auto itemName = ItemRegistry::GetName(itemId);
        ImGui::PushID(itemId);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pos = ImGui::GetCursorScreenPos();

        // Icon
        auto uv = renderlib::TextureAtlas::GetItemUV(itemId);
        float iconOffsetX = (cellW - kIconSize) * 0.5f;
        dl->AddImage(
            ImTextureID(static_cast<ImTextureID>(renderlib::TextureAtlas::GetTextureHandle().idx)),
            ImVec2(pos.x + iconOffsetX, pos.y),
            ImVec2(pos.x + iconOffsetX + kIconSize, pos.y + kIconSize),
            ImVec2(uv.u0, uv.v0),
            ImVec2(uv.u1, uv.v1));

        // Click/hover area spanning full cell width
        ImGui::InvisibleButton("cell", ImVec2(cellW, kIconSize + nameH + 2.0f));

        if (ImGui::IsItemHovered()) {
            if (uiMgr_ && uiMgr_->GetPlayerInventory()) {
                uiMgr_->GetPlayerInventory()->hoveredItemId = itemId;
            }
            char tip[128];
            std::snprintf(tip, sizeof(tip), "%s (ID: %u)", std::string(itemName).c_str(), itemId);
            ImGui::SetTooltip("%s", tip);
        }

        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && uiMgr_) {
            uint8_t count = ItemRegistry::GetStackSize(itemId);
            uiMgr_->GetActions().SpawnItem(itemId, count, spawnTargetSlot());
        }

        // Name below icon (truncated to fit cell)
        ImVec2 namePos(pos.x + 2.0f, pos.y + kIconSize + 1.0f);
        ImVec2 nameMax(pos.x + cellW - 2.0f, namePos.y + nameH);
        std::string nameStr(itemName);
        if (ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, nameMax.x - namePos.x, nameStr.c_str()).x > (nameMax.x - namePos.x)) {
            while (!nameStr.empty()) {
                auto shortName = nameStr.substr(0, nameStr.size() - 1) + ".";
                if (ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, nameMax.x - namePos.x, shortName.c_str()).x <= (nameMax.x - namePos.x)) {
                    nameStr = shortName;
                    break;
                }
                nameStr.pop_back();
            }
        }
        dl->AddText(namePos, IM_COL32(200, 200, 200, 220), nameStr.c_str());

        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::End();
}
