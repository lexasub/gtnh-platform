#pragma once

#include <string>
#include <vector>
#include <cstdio>
#include <algorithm>
#include <functional>
#include <imgui.h>

#include "Common/Inventory.h"
#include "Components/SlotGrid.h"
#include "Crafting/ClientItemRegistry.h"

struct ImDrawList;
struct ImVec2;

struct RecipeEntry {
    std::string group;
    std::string name;
    std::vector<ItemStack> inputs;
    std::vector<ItemStack> outputs;
    uint32_t duration;
};

// Renders a single recipe entry with input slots, arrow, output slots, duration, and separator.
// slotIdx is incremented for each slot rendered to ensure unique PushID values.
inline void RenderRecipeEntry(const RecipeEntry& entry, int& slotIdx,
                               std::function<void(const ItemStack&)> onClick = nullptr) {
    // Title + machine group
    ImGui::Text("%s", entry.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", entry.group.c_str());

    // Input slots
    for (const auto& input : entry.inputs) {
        ImGui::PushID(slotIdx++);
        if (RenderSlot(input, false, ImGui::GetWindowDrawList())) {
            if (onClick) onClick(input);
        }
        ImGui::PopID();
        ImGui::SameLine();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", ItemRegistry::GetName(input.item_id).data());
    }

    // Arrow
    if (!entry.inputs.empty()) {
        ImGui::SameLine();
        ImGui::Text(" \xE2\x86\x92 ");
        ImGui::SameLine();
    }

    // Output slots
    for (const auto& output : entry.outputs) {
        ImGui::PushID(slotIdx++);
        if (RenderSlot(output, false, ImGui::GetWindowDrawList())) {
            if (onClick) onClick(output);
        }
        ImGui::PopID();
        ImGui::SameLine();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", ItemRegistry::GetName(output.item_id).data());
    }

    // Duration
    if (entry.duration > 0) {
        char dur[32];
        std::snprintf(dur, sizeof(dur), "Duration: %u ticks", entry.duration);
        ImGui::Text("%s", dur);
    }

    ImGui::Separator();
}
