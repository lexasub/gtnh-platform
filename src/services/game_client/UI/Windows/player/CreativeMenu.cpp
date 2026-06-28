#include "CreativeMenu.h"
#include "UIManager.h"
#include "Core/ActionHandler.h"
#include "Crafting/ClientItemRegistry.h"
#include "machine_registry/MachineRegistry.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <algorithm>

CreativeMenu::CreativeMenu(UIManager* mgr)
    : uiMgr_{mgr} {}

void CreativeMenu::rebuildItemList() {
    items_.clear();

    // Load from ItemRegistry (items.csv)
    for (auto id : ItemRegistry::GetAllItemIds()) {
        if (id == 0) continue; // skip air
        const auto* info = ItemRegistry::GetItem(id);
        if (!info) continue;
        items_.push_back({id, info->name, false});
    }

    // Load from MachineRegistry (machines.yaml)
    if (auto* reg = MachineRegistry::instance()) {
        for (auto& [id, info] : reg->All()) {
            // Check if already added from ItemRegistry
            bool found = false;
            for (auto& item : items_) {
                if (item.id == id) { found = true; break; }
            }
            if (!found) {
                items_.push_back({id, info.name, true});
            }
        }
    }

    std::sort(items_.begin(), items_.end(),
              [](const Item& a, const Item& b) { return a.id < b.id; });
}

bool CreativeMenu::OnKeyEvent(int key, int /*action*/, int /*mods*/) {
    if (key == GLFW_KEY_TAB) {
        open_ = !open_;
        if (open_) rebuildItemList();
        return true;
    }
    return false;
}

void CreativeMenu::Render(InventoryState* /*playerInv*/) {
    if (!open_) return;

    ImGui::Begin("Creative Menu", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::InputText("##search", searchBuf_, sizeof(searchBuf_));

    ImGui::BeginChild("items", ImVec2(300, 400), true);
    for (const auto& item : items_) {
        if (searchBuf_[0]) {
            // Search by name (case-insensitive) or ID
            bool match = false;
            std::string lowerName = item.name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
            std::string lowerSearch = searchBuf_;
            std::transform(lowerSearch.begin(), lowerSearch.end(), lowerSearch.begin(), ::tolower);
            if (lowerName.find(lowerSearch) != std::string::npos) match = true;
            char idStr[16];
            std::snprintf(idStr, sizeof(idStr), "%u", item.id);
            if (strstr(idStr, searchBuf_)) match = true;
            if (!match) continue;
        }
        ImGui::PushID(item.id);
        if (ImGui::Selectable(item.name.c_str(), selectedItem_ == item.id))
            selectedItem_ = item.id;
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::InputInt("Count", &count_);
    count_ = std::clamp(count_, 1, 64);

    if (ImGui::Button("Spawn", ImVec2(100, 0))) {
        if (selectedItem_ > 0 && uiMgr_) {
            uiMgr_->GetActions().SpawnItem(selectedItem_, static_cast<uint8_t>(count_), -1);
        }
    }

    ImGui::End();
}
