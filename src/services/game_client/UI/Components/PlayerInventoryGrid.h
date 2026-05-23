#pragma once

#include <algorithm>

#include "Components/SlotGrid.h"
#include "UI/Core/DragManager.h"
#include "Common/Inventory.h"
#include <imgui.h>

// ── RenderPlayerInventoryGrid — boilerplate player inventory grid ──────────
// Creates a SlotGridComponent with resolution-aware styling and renders the
// given range of slots.  Slot size scales with display height (baseline 1080p)
// so the inventory grid is usable at any resolution.
//
// Returns the clicked slot index, or -1.
inline int RenderPlayerInventoryGrid(InventoryState& inv, int startIndex, int count,
                                     int columns, int selectedSlot,
                                     bool /*enableDragPreview*/ = false,
                                     DragManager* dragMgr = nullptr) {
    // Scale slot size to display resolution (baseline 1080p, ~40 px slots)
    float displayH = ImGui::GetIO().DisplaySize.y;
    float scale = std::max(0.7f, std::min(1.5f, displayH / 1080.0f));
    int slotSize = std::max(20, static_cast<int>(40.0f * scale));
    int padding  = std::max(1, static_cast<int>(2.0f * scale));

    SlotGridComponent grid(inv.slots);
    SlotStyle style;
    style.size = slotSize;
    style.padding = padding;
    style.showNumbers = true;
    style.drawBackground = true;
    style.backgroundColor = 0;
    style.selectedColor = 0;
    style.selectedBorder = 0;

    grid.SetStyle(style);
    grid.SetRange(startIndex, count, columns);
    grid.SetSelectedSlot(selectedSlot);
    grid.SetInventory(inv);
    if (dragMgr) grid.SetDragManager(dragMgr);

    return grid.Render();
}
