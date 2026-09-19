#pragma once

#include <cstdint>
#include <string>
#include <vector>

class ItemIndex {
  std::vector<uint16_t> items_;     // current view (filtered)
  std::vector<uint16_t> fullItems_; // complete sorted list
public:
  ItemIndex() = default;

  void Rebuild();

  void SetSearch(const std::string &query);

  const std::vector<uint16_t> &GetItems() const { return items_; }
};
