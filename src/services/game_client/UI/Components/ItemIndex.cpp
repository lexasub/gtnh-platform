#include "UI/Components/ItemIndex.h"
#include "Crafting/ClientItemRegistry.h"
#include <algorithm>
#include <cctype>

void ItemIndex::Rebuild() {
    fullItems_ = ItemRegistry::GetAllItemIds();
    std::sort(fullItems_.begin(), fullItems_.end(),
              [](uint16_t a, uint16_t b) {
                  return ItemRegistry::GetName(a) < ItemRegistry::GetName(b);
              });
    items_ = fullItems_;
}

void ItemIndex::SetSearch(const std::string& query) {
    if (query.empty()) {
        items_ = fullItems_;
        return;
    }
    std::string lower = query;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    items_ = fullItems_;
    std::erase_if(items_, [&](uint16_t id) {
        auto name = std::string(ItemRegistry::GetName(id));
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        return name.find(lower) == std::string::npos;
    });
}
