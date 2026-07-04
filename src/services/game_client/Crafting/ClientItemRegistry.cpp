#include "Crafting/ClientItemRegistry.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <common/ItemId.h>

namespace ItemRegistry {

static std::unordered_map<uint16_t, ItemInfo> s_items;

void Init() {
    LoadFromCSV("data/registry/items.csv");
}

void LoadFromCSV(const std::string& csvPath) {
    std::ifstream file(csvPath);
    if (!file.is_open()) {
        std::cerr << "Failed to open items.csv: " << csvPath << std::endl;
        return;
    }

    std::string line;
    if (!std::getline(file, line)) {
        return;
    }

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::istringstream iss(line);
        std::string field;

        // int (prefix notation or flat decimal)
        if (!std::getline(iss, field, ',')) continue;
        uint16_t id = ItemId::pack(field);
        if (id == 0 && field != "0" && field != "0:0:0") continue;

        if (id == 0) continue;

        // name
        if (!std::getline(iss, field, ',')) continue;
        std::string name = field;

        // stack (optional, default 64)
        uint8_t stackSize = 64;
        if (std::getline(iss, field, ',') && !field.empty()) {
            try {
                stackSize = static_cast<uint8_t>(std::stoul(field));
            } catch (const std::exception&) {
                // keep default
            }
        }

        // meta (optional, default 0)
        uint16_t meta = 0;
        if (std::getline(iss, field, ',') && !field.empty()) {
            try {
                meta = static_cast<uint16_t>(std::stoul(field));
            } catch (const std::exception&) {
                // keep default
            }
        }

        s_items[id] = {id, std::move(name), stackSize, meta};
    }

    std::cout << "Loaded " << s_items.size() << " items from " << csvPath << std::endl;
}

const ItemInfo* GetItem(uint16_t itemId) {
    auto it = s_items.find(itemId);
    return (it != s_items.end()) ? &it->second : nullptr;
}

std::string_view GetName(uint16_t itemId) {
    if (auto item = GetItem(itemId)) {
        return item->name;
    }
    return "???";
}

uint8_t GetStackSize(uint16_t itemId) {
    if (auto item = GetItem(itemId)) {
        return item->stackSize;
    }
    return 64;
}

std::vector<uint16_t> GetAllItemIds() {
    std::vector<uint16_t> ids;
    ids.reserve(s_items.size());
    for (const auto& [id, _] : s_items) {
        ids.push_back(id);
    }
    return ids;
}

}
