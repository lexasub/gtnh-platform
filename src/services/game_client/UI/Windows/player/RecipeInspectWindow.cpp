#include "RecipeInspectWindow.h"
#include "Crafting/ClientMachineRecipeDB.h"
#include "Crafting/ClientRecipeDB.h"
#include "Crafting/ClientItemRegistry.h"
#include "Components/SlotGrid.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <algorithm>
#include <cstdio>

RecipeInspectWindow::RecipeInspectWindow() {}

void RecipeInspectWindow::SetItem(uint16_t itemId) {
    if (itemId == itemId_) return;
    itemId_ = itemId;
    activeTab_ = 0;
    page_ = 0;
    rebuildEntries();
}

void RecipeInspectWindow::rebuildEntries() {
    recipes_.clear();
    uses_.clear();
    page_ = 0;

    if (itemId_ == 0) return;

    struct TempEntry {
        std::string group;
        std::string name;
        std::vector<ItemStack> inputs;
        std::vector<ItemStack> outputs;
        uint32_t duration;
        std::vector<uint16_t> outputIds;
        std::vector<uint16_t> inputIds;
    };
    std::vector<TempEntry> all;

    for (const auto& rec : Crafting::kRecipes) {
        if (rec.output.item_id == 0) continue;
        TempEntry e;
        e.group = "Crafting Table";
        e.name = std::string(ItemRegistry::GetName(rec.output.item_id));
        for (int i = 0; i < 9; ++i) {
            if (rec.input_slots[i].item_id != 0)
                e.inputs.push_back(rec.input_slots[i]);
        }
        e.outputs.push_back(rec.output);
        e.duration = 0;
        e.outputIds.push_back(rec.output.item_id);
        for (const auto& in : e.inputs) e.inputIds.push_back(in.item_id);
        all.push_back(std::move(e));
    }

    for (auto& [type, recs] : MachineRecipes::s_recipes) {
        for (const auto& rec : recs) {
            TempEntry e;
            e.group = std::to_string(type);
            e.name = rec.name;
            e.outputs = rec.outputs;
            e.inputs = rec.inputs;
            e.duration = rec.duration;
            for (const auto& out : rec.outputs) e.outputIds.push_back(out.item_id);
            for (const auto& in : rec.inputs) e.inputIds.push_back(in.item_id);
            all.push_back(std::move(e));
        }
    }

    for (const auto& e : all) {
        bool isRecipe = std::find(e.outputIds.begin(), e.outputIds.end(), itemId_) != e.outputIds.end();
        bool isUse = std::find(e.inputIds.begin(), e.inputIds.end(), itemId_) != e.inputIds.end();

        if (isRecipe) {
            RecipeEntry out;
            out.group = e.group;
            out.name = e.name;
            out.inputs = e.inputs;
            out.outputs = e.outputs;
            out.duration = e.duration;
            recipes_.push_back(std::move(out));
        }
        if (isUse) {
            RecipeEntry out;
            out.group = e.group;
            out.name = e.name;
            out.inputs = e.inputs;
            out.outputs = e.outputs;
            out.duration = e.duration;
            uses_.push_back(std::move(out));
        }
    }
}

void RecipeInspectWindow::renderTabContent(const std::vector<RecipeEntry>& entries) {
    if (entries.empty()) {
        ImGui::Text("No %s found", activeTab_ == 0 ? "recipes" : "uses");
        return;
    }

    int slotIdx = 0;
    RenderPaginatedList(entries, page_, kPerPage,
        [&slotIdx](const RecipeEntry& entry, int) {
            RenderRecipeEntry(entry, slotIdx);
        },
        "No entries");
}

void RecipeInspectWindow::Render([[maybe_unused]] InventoryState* playerInv) {
    if (!open_ || itemId_ == 0) return;

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

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        open_ = false;
    }
}
