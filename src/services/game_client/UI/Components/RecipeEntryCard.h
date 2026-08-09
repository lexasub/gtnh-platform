#pragma once

#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <imgui.h>
#include <string>
#include <vector>

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
  // Positional 3x3 pattern (crafting-table recipes). When set, inputs are
  // rendered as a 3x3 grid instead of a single row.
  bool has_pattern = false;
  std::array<ItemStack, 9> pattern{};
};

// Renders a single recipe entry with input slots, arrow, output slots,
// duration, and separator. slotIdx is incremented for each slot rendered to
// ensure unique PushID values.
// When hoveredItemId is non-null, it is written with the item id of whichever
// slot is under the mouse each frame (0 when no slot is hovered) — used by
// RecipeInspectWindow for R/U drill-in.
inline void
RenderRecipeEntry(const RecipeEntry &entry, int &slotIdx,
                  std::function<void(const ItemStack &)> onClick = nullptr,
                  uint16_t *hoveredItemId = nullptr) {
  // Title + machine group
  ImGui::Text("%s", entry.name.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("(%s)", entry.group.c_str());

  // Input slots: 3x3 pattern for crafting recipes, flat row otherwise.
  if (entry.has_pattern) {
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) {
        int cell = row * 3 + col;
        const ItemStack &stack = entry.pattern[cell];
        ImGui::PushID(slotIdx++);
        if (RenderSlot(stack, false, ImGui::GetWindowDrawList())) {
          if (onClick && stack.item_id != 0)
            onClick(stack);
        }
        ImGui::PopID();
        if (col < 2)
          ImGui::SameLine();
        if (ImGui::IsItemHovered()) {
          if (hoveredItemId)
            *hoveredItemId = stack.item_id;
          if (stack.item_id != 0)
            ImGui::SetTooltip("%s", ItemRegistry::GetName(stack.item_id).data());
        }
      }
    }
  } else {
    for (const auto &input : entry.inputs) {
      ImGui::PushID(slotIdx++);
      if (RenderSlot(input, false, ImGui::GetWindowDrawList())) {
        if (onClick)
          onClick(input);
      }
      ImGui::PopID();
      ImGui::SameLine();
      if (ImGui::IsItemHovered()) {
        if (hoveredItemId)
          *hoveredItemId = input.item_id;
        if (input.item_id != 0)
          ImGui::SetTooltip("%s", ItemRegistry::GetName(input.item_id).data());
      }
    }
  }

  // Arrow
  if (!entry.inputs.empty()) {
    ImGui::SameLine();
    ImGui::Text(" \xE2\x86\x92 ");
    ImGui::SameLine();
  }

  // Output slots
  for (const auto &output : entry.outputs) {
    ImGui::PushID(slotIdx++);
    if (RenderSlot(output, false, ImGui::GetWindowDrawList())) {
      if (onClick)
        onClick(output);
    }
    ImGui::PopID();
    ImGui::SameLine();
    if (ImGui::IsItemHovered()) {
      if (hoveredItemId)
        *hoveredItemId = output.item_id;
      if (output.item_id != 0)
        ImGui::SetTooltip("%s", ItemRegistry::GetName(output.item_id).data());
    }
  }

  // Duration
  if (entry.duration > 0) {
    char dur[32];
    std::snprintf(dur, sizeof(dur), "Duration: %u ticks", entry.duration);
    ImGui::Text("%s", dur);
  }

  ImGui::Separator();
}
