#include "Components/SlotGrid.h"
#include "UI/Core/DragManager.h"
#include "Common/Inventory.h"
#include "Crafting/ClientItemRegistry.h"
#include "RenderLib/Utils/TextureAtlas.h"
#include <data/registry/ToolIds.h>
#include <imgui.h>
#include <cstdio>
#include <algorithm>
#include <spdlog/spdlog.h>

// ── Default colors ──────────────────────────────────────────────────────────
namespace {
    uint32_t DefaultBg()      { return IM_COL32(70, 70, 70, 200); }
    uint32_t DefaultSelBg()   { return IM_COL32(255, 255, 255, 60); }
    uint32_t DefaultSelBrd()  { return IM_COL32(255, 255, 255, 200); }
    uint32_t DefaultBorder()  { return IM_COL32(110, 110, 110, 220); }

    SlotStyle NormalizedStyle(const SlotStyle& s) {
        SlotStyle out = s;
        if (out.backgroundColor == 0) out.backgroundColor = DefaultBg();
        if (out.selectedColor == 0)   out.selectedColor   = DefaultSelBg();
        if (out.selectedBorder == 0)  out.selectedBorder  = DefaultSelBrd();
        if (out.borderColor == 0)     out.borderColor     = DefaultBorder();
        return out;
    }
}

// ── RenderSlot ──────────────────────────────────────────────────────────────
bool RenderSlot(const ItemStack& stack, bool selected,
                ImDrawList* dl, const SlotStyle& style) {
    SlotStyle s = NormalizedStyle(style);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 slotSize(static_cast<float>(s.size), static_cast<float>(s.size));

    uint32_t bg = selected ? s.selectedColor : s.backgroundColor;
    if (s.drawBackground) {
        dl->AddRectFilled(pos, ImVec2(pos.x + slotSize.x, pos.y + slotSize.y),
                          bg, 2.0f);
    }

    float bt = static_cast<float>(s.borderThickness);
    dl->AddRect(pos, ImVec2(pos.x + slotSize.x, pos.y + slotSize.y),
                s.borderColor, 2.0f, 0, bt);

    if (selected) {
        dl->AddRect(pos, ImVec2(pos.x + slotSize.x, pos.y + slotSize.y),
                    s.selectedBorder, 2.0f, 0, 2.0f);
    }

    if (stack.item_id != 0) {
        uint32_t itemColor = IM_COL32(
            stack.item_id * 50 % 256,
            stack.item_id * 80 % 256,
            stack.item_id * 30 % 256, 255);
        dl->AddRectFilled(ImVec2(pos.x + 4, pos.y + 4),
                          ImVec2(pos.x + s.size - 4, pos.y + s.size - 4),
                          itemColor, 2.0f);
        if (s.showNumbers && stack.count > 1) {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%d", stack.count);
            dl->AddText(ImVec2(pos.x + 4, pos.y + 4),
                        IM_COL32(255, 255, 255, 255), buf);
        }
    }

    ImGui::InvisibleButton("slot", slotSize);
    bool activated = ImGui::IsItemActivated();
    if (activated) {
        spdlog::info("SlotGrid: slot activated at screen ({:.0f},{:.0f})",
                      pos.x, pos.y);
    }
    return activated;
}

// ── RenderSlotGrid ──────────────────────────────────────────────────────────
int RenderSlotGrid(std::vector<ItemStack>& slots,
                   int startIndex, int count, int cols,
                   int selectedSlot, const SlotStyle& style,
                   std::function<void(int, int, bool)>* clickCb,
                   DragManager* dragMgr) {
    spdlog::info("RenderSlotGrid entered start={} count={} dragMgr={}", startIndex, count, dragMgr != nullptr);
    int clickedSlot = -1;
    int end = std::min(startIndex + count, static_cast<int>(slots.size()));

    for (int i = startIndex; i < end; ++i) {
        int col = (i - startIndex) % cols;
        if (col > 0) ImGui::SameLine();

        int globalIdx = i;
        bool selected = (globalIdx == selectedSlot);
        ImGui::PushID(globalIdx);
        if (RenderSlot(slots[i], selected, ImGui::GetWindowDrawList(), style)) {
            clickedSlot = globalIdx;
            if (clickCb) {
                int button = ImGui::IsMouseClicked(ImGuiMouseButton_Right) ? 1 : 0;
                bool shift = ImGui::GetIO().KeyShift;
                (*clickCb)(globalIdx, button, shift);
            }
            if (dragMgr) {
                int button = ImGui::IsMouseClicked(ImGuiMouseButton_Right) ? 1 : 0;
                bool shift = ImGui::GetIO().KeyShift;
                dragMgr->OnSlotActivated(globalIdx, slots, button, shift);
            }
        }
        ImGui::PopID();
    }

    return clickedSlot;
}

// ── RenderHotbar ────────────────────────────────────────────────────────────
int RenderHotbar(const std::vector<ItemStack>& slots, int selectedSlot,
                 const SlotStyle& style, DragManager* dragMgr) {
    SlotStyle s = NormalizedStyle(style);
    constexpr int kHotbarSlots = 10;

    float w = ImGui::GetIO().DisplaySize.x;
    float h = ImGui::GetIO().DisplaySize.y;
    float totalW = kHotbarSlots * static_cast<float>(s.size + s.padding);
    float startX = (w - totalW) / 2.0f;
    float y = h - s.size - 20.0f;

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // Background panel
    dl->AddRectFilled(ImVec2(startX - 4.0f, y - 4.0f),
                      ImVec2(startX + totalW + 4.0f, y + s.size + 4.0f),
                      IM_COL32(0, 0, 0, 160), 4.0f);

    ImVec2 mouse = ImGui::GetIO().MousePos;
    int hoveredSlot = -1;

    for (int i = 0; i < kHotbarSlots; ++i) {
        float x = startX + i * static_cast<float>(s.size + s.padding);
        ImVec2 pos(x, y);
        ImVec2 slotSize(static_cast<float>(s.size), static_cast<float>(s.size));

        bool selected = (i == selectedSlot);
        uint32_t bg = selected ? s.selectedColor : s.backgroundColor;
        dl->AddRectFilled(pos, ImVec2(pos.x + slotSize.x, pos.y + slotSize.y),
                          bg, 2.0f);
        float bt = static_cast<float>(s.borderThickness);
        dl->AddRect(pos, ImVec2(pos.x + slotSize.x, pos.y + slotSize.y),
                    s.borderColor, 2.0f, 0, bt);
        if (selected) {
            dl->AddRect(pos, ImVec2(pos.x + slotSize.x, pos.y + slotSize.y),
                        s.selectedBorder, 2.0f, 0, 2.0f);
        }

        if (static_cast<size_t>(i) < slots.size() && slots[i].item_id != 0) {
            auto uv = renderlib::TextureAtlas::GetItemUV(slots[i].item_id);
            dl->AddImage(
                renderlib::TextureAtlas::GetTextureHandle().idx,
                ImVec2(pos.x + 4, pos.y + 4),
                ImVec2(pos.x + s.size - 4, pos.y + s.size - 4),
                ImVec2(uv.u0, uv.v0),
                ImVec2(uv.u1, uv.v1));
            if (s.showNumbers && slots[i].count > 1) {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%d", slots[i].count);
                dl->AddText(ImVec2(pos.x + 4, pos.y + 4),
                            IM_COL32(255, 255, 255, 255), buf);
            }
        }

        // Key number
        char key[2] = {(i < 9) ? static_cast<char>('1' + i) : '0', 0};
        dl->AddText(ImVec2(pos.x + 4, pos.y + s.size + 2),
                    IM_COL32(200, 200, 200, 180), key);

        // Hit test
        if (mouse.x >= pos.x && mouse.x <= pos.x + slotSize.x &&
            mouse.y >= pos.y && mouse.y <= pos.y + slotSize.y) {
            hoveredSlot = i;
        }
    }

    if (dragMgr && hoveredSlot >= 0) {
        dragMgr->UpdateHover(hoveredSlot);
    }

    return hoveredSlot;
}

// ── SlotGridComponent ───────────────────────────────────────────────────────
SlotGridComponent::SlotGridComponent(std::vector<ItemStack>& slots)
    : slots_(slots) {}

void SlotGridComponent::SetRange(int startIndex, int count, int cols) {
    startIndex_ = startIndex;
    count_ = count;
    cols_ = cols;
}

// ── SetInventory ───────────────────────────────────────────────────────────
void SlotGridComponent::SetInventory(InventoryState& inv) {
    inv_ = &inv;
}

// ── Render ──────────────────────────────────────────────────────────────────
int SlotGridComponent::Render() {
    int clicked = -1;
    int end = std::min(startIndex_ + count_, static_cast<int>(slots_.size()));
    SlotStyle s = NormalizedStyle(style_);
    //TODO refactor if hall
    for (int i = startIndex_; i < end; ++i) {
        int col = (i - startIndex_) % cols_;
        if (col > 0) ImGui::SameLine();

        int globalIdx = i;
        bool selected = (globalIdx == selectedSlot_);
        ImGui::PushID(globalIdx);

        ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImVec2 slotSize(static_cast<float>(s.size),
                        static_cast<float>(s.size));
        ImGui::InvisibleButton("slot", slotSize);

        // ── Track hover ─────────────────────────────────────────────────────
        if (ImGui::IsItemHovered()) {
            hoveredSlot_ = globalIdx;
            inv_->dragHoverSlot = globalIdx;
            inv_->hoveredSlot = static_cast<int16_t>(globalIdx);
            inv_->hoveredItemId = slots_[globalIdx].item_id;

            uint16_t id = slots_[globalIdx].item_id;
            if (id != 0) {
                uint16_t meta = slots_[globalIdx].meta;
                uint8_t cnt = slots_[globalIdx].count;
                auto name = ItemRegistry::GetName(id);
                ImGui::BeginTooltip();
                ImGui::Text("%.*s", static_cast<int>(name.size()), name.data());
                if (cnt > 1) {
                    ImGui::Text("x%d", cnt);
                }
                // Tool energy display
                uint32_t capacity = 0;
                const char* toolName = nullptr;
                switch (id) {
                    case ITEM_DRILL_ULV:  capacity = 1000;  toolName = "Drill (ULV)"; break;
                    case ITEM_DRILL_LV:   capacity = 4000;  toolName = "Drill (LV)"; break;
                    case ITEM_DRILL_MV:   capacity = 16000; toolName = "Drill (MV)"; break;
                    case ITEM_DRILL_HV:   capacity = 64000; toolName = "Drill (HV)"; break;
                    case ITEM_CHAINSAW_LV: capacity = 4000; toolName = "Chainsaw (LV)"; break;
                    case ITEM_WRENCH:     toolName = "Wrench"; break;
                }
                if (capacity > 0) {
                    float pct = (meta * 100.0f) / capacity;
                    ImGui::Text("%s", toolName);
                    ImGui::Text("Energy: %u / %u EU", meta, capacity); //EU? may be recognize current energy type? (ex - benz saw)
                    ImGui::ProgressBar(pct / 100.0f, ImVec2(120, 0), "");
                } else if (toolName) {
                    ImGui::Text("%s", toolName);
                }
                if (meta != 0 && capacity == 0) {
                    ImGui::Text("Damage: %u", meta);
                }
                ImGui::TextDisabled("ID %u", id);
                ImGui::EndTooltip();
            }
        }

        // ── Drag state machine ──────────────────────────────────────────────
        if (dm_ && ImGui::IsItemActivated()) {
            int button = ImGui::IsMouseClicked(ImGuiMouseButton_Right) ? 1 : 0;
            bool shift = ImGui::GetIO().KeyShift;
            dm_->OnSlotActivated(globalIdx, slots_, button, shift);
            clicked = globalIdx;
        }

        // ── Render slot background ────────────────────────────────────────
        uint32_t bg = selected ? s.selectedColor : s.backgroundColor;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (s.drawBackground) {
            dl->AddRectFilled(cursor, ImVec2(cursor.x + slotSize.x, cursor.y + slotSize.y),
                              bg, 2.0f);
        }
        float bt = static_cast<float>(s.borderThickness);
        dl->AddRect(cursor, ImVec2(cursor.x + slotSize.x, cursor.y + slotSize.y),
                    s.borderColor, 2.0f, 0, bt);
        if (selected) {
            dl->AddRect(cursor, ImVec2(cursor.x + slotSize.x, cursor.y + slotSize.y),
                        s.selectedBorder, 2.0f, 0, 2.0f);
        }

        // ── Item icon ──────────────────────────────────────────────────
        if (slots_[globalIdx].item_id != 0) {
            auto uv = renderlib::TextureAtlas::GetItemUV(slots_[globalIdx].item_id);
            dl->AddImage(
                renderlib::TextureAtlas::GetTextureHandle().idx,
                ImVec2(cursor.x + 4, cursor.y + 4),
                ImVec2(cursor.x + s.size - 4, cursor.y + s.size - 4),
                ImVec2(uv.u0, uv.v0),
                ImVec2(uv.u1, uv.v1));
            if (s.showNumbers && slots_[globalIdx].count > 1) {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%d", slots_[globalIdx].count);
                dl->AddText(ImVec2(cursor.x + 4, cursor.y + 4),
                            IM_COL32(255, 255, 255, 255), buf);
            }
        }

        ImGui::PopID();
    }

    if (dm_ && dm_->IsDragging()) {
        dm_->RenderPreview(s);
    }

    // ── ESC cancels drag (returns item to source) ────────────────────────
    if (dm_ && dm_->IsDragging() && ImGui::IsKeyPressed(ImGuiKey_Escape)) { //TODO concrete button via uidefaults
        dm_->CancelDrag(slots_);
    }

    // ── Q while dragging → drop (destroy) held item ─────────────────────
    if (dm_ && dm_->IsDragging() && ImGui::IsKeyPressed(ImGuiKey_Q)) {//TODO concrete button via uidefaults
        dm_->DropHeldItem();
    }

    // ── Q while hovering an item (not dragging) → drop that slot ────────
    // Uses DragManager's callback (wired to SendInventoryAction) to notify server.
    if (dm_ && !dm_->IsDragging() && inv_ && inv_->hoveredSlot >= 0
        && ImGui::IsKeyPressed(ImGuiKey_Q))//TODO concrete button via uidefaults
    {
        auto& slot = slots_[inv_->hoveredSlot];
        if (slot.item_id != 0) {
            ItemStack dropped = slot;
            slot = ItemStack{};
            dm_->StartExternalDrag(inv_->hoveredSlot, dropped);
            dm_->DropHeldItem();
        }
    }

    if (dm_ && dm_->IsDragging() && !inv_->dropEnabled) {
        dm_->CancelDrag(slots_);
    }

    return clicked;
}
