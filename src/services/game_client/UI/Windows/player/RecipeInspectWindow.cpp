#include "RecipeInspectWindow.h"
#include "UIManager.h"
#include "Crafting/ServerRecipeDB.h"
#include "Crafting/ClientItemRegistry.h"
#include "Components/SlotGrid.h"
#include "UI/Core/InputBinder.h"
#include "RenderLib/UI/ImGuiKeyMap.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <algorithm>
#include <cstdio>

RecipeInspectWindow::RecipeInspectWindow(UIManager *uiMgr) : uiMgr_(uiMgr) {}

void RecipeInspectWindow::SetItem(uint16_t itemId) {
    if (itemId == itemId_) return;
    itemId_ = itemId;
    activeTab_ = 0;
    page_ = 0;
    recipes_.clear();
    uses_.clear();
    rebuildEntries();
}

void RecipeInspectWindow::rebuildEntries() {
    if (itemId_ == 0 || !uiMgr_) return;
    auto *db = uiMgr_->GetRecipeDb();
    if (!db) return;
    uint16_t id = itemId_;
    // Async: server returns every recipe involving this item (both directions);
    // rebuildFromServer splits into "crafted as output" / "used as input".
    db->GetRecipesForItem(id, [this, id]() {
        if (id != itemId_) return; // user switched items while in flight
        rebuildFromServer();
    });
}

void RecipeInspectWindow::rebuildFromServer() {
    recipes_.clear();
    uses_.clear();
    if (!uiMgr_ || itemId_ == 0) return;
    auto *db = uiMgr_->GetRecipeDb();
    if (!db) return;
    auto itemRecipes = db->GetItemRecipesCopy(itemId_);
    uint8_t playerEra = uiMgr_->GetCurrentEra();

    auto makeEntry = [](const ServerRecipeDB::RecipeInfo &ri) -> RecipeEntry {
        RecipeEntry e;
        e.group = ri.machine_class.empty()
                      ? (ri.machine_type ? std::to_string(ri.machine_type)
                                         : "Crafting Table")
                      : ri.machine_class;
        e.name = ri.outputs.empty()
                     ? ri.recipe_id
                     : std::string(ItemRegistry::GetName(ri.outputs[0].item_id));
        e.inputs = ri.inputs;
        e.outputs = ri.outputs;
        e.duration = ri.duration;
        e.has_pattern = ri.has_pattern;
        e.pattern = ri.pattern;
        return e;
    };

    for (const auto &ri : itemRecipes.craft) {
        if (ri.unlock_era > playerEra) continue; // locked until quest era advances
        recipes_.push_back(makeEntry(ri));
    }
    for (const auto &ri : itemRecipes.use) {
        if (ri.unlock_era > playerEra) continue;
        uses_.push_back(makeEntry(ri));
    }
}

void RecipeInspectWindow::renderTabContent(const std::vector<RecipeEntry>& entries) {
    if (entries.empty()) {
        ImGui::Text("No %s found", activeTab_ == 0 ? "recipes" : "uses");
        return;
    }

    int slotIdx = 0;
    RenderPaginatedList(entries, page_, kPerPage,
        [this, &slotIdx](const RecipeEntry& entry, int) {
            RenderRecipeEntry(entry, slotIdx, nullptr, &hoveredItem_);
        },
        "No entries");
}

void RecipeInspectWindow::Render([[maybe_unused]] InventoryState* playerInv) {
    if (!open_ || itemId_ == 0) return;

    // Refresh the drill-in target each frame: only a slot hovered THIS frame
    // may be drilled into (stale hovers must not survive R/U).
    hoveredItem_ = 0;

    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(550, 460), ImGuiCond_Always);

    auto itemName = ItemRegistry::GetName(itemId_);
    char title[64];
    std::snprintf(title, sizeof(title), "%.*s", static_cast<int>(itemName.size()), itemName.data());

    ImGui::Begin(title, nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    if (ImGui::BeginTabBar("recipeTabs")) {
        if (ImGui::BeginTabItem("Recipes")) {
            if (activeTab_ != 0) { activeTab_ = 0; page_ = 0; }
            renderTabContent(recipes_);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Uses")) {
            if (activeTab_ != 1) { activeTab_ = 1; page_ = 0; }
            renderTabContent(uses_);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();

    if (ImGui::IsKeyPressed(renderlib::GLFWKeyToImGuiKey(
            uiMgr_ ? uiMgr_->GetBinder().GetKey("close_ui") : -1))) {
        open_ = false;
    }
}

void RecipeInspectWindow::DrillInto(int tab) {
    if (hoveredItem_ == 0) return;
    if (hoveredItem_ != itemId_) {
        SetItem(hoveredItem_); // resets to the Recipes tab + rebuilds
    }
    activeTab_ = tab; // 0 = Recipes, 1 = Uses
    page_ = 0;
}
