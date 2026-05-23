#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace ItemRegistry {

struct ItemInfo {
    uint16_t id;
    std::string name;
    uint8_t stackSize;
    uint16_t meta;
};

void Init();
void LoadFromCSV(const std::string& csvPath);
const ItemInfo* GetItem(uint16_t itemId);
std::string_view GetName(uint16_t itemId);
uint8_t GetStackSize(uint16_t itemId);
std::vector<uint16_t> GetAllItemIds();

}
