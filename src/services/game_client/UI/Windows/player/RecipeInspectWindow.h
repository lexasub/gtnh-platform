#pragma once

#include "Common/Inventory.h"
#include "Components/PaginatedList.h"
#include "Components/RecipeEntryCard.h"
#include "Windows/IUIWindow.h"
#include <cstdint>
#include <string>
#include <vector>

class RecipeInspectWindow : public IUIWindow {
public:
  RecipeInspectWindow();

  std::string_view Name() const override { return "RecipeInspect"; }
  void Render(InventoryState *playerInv) override;

  bool IsOpen() const override { return open_; }
  void SetOpen(bool open) override { open_ = open; }

  void SetItem(uint16_t itemId);

private:
  bool open_ = false;
  uint16_t itemId_ = 0;
  int activeTab_ = 0; // 0 = Recipes, 1 = Uses
  int page_ = 0;
  static constexpr int kPerPage = 8;

  std::vector<RecipeEntry> recipes_;
  std::vector<RecipeEntry> uses_;

  void rebuildEntries();
  void renderTabContent(const std::vector<RecipeEntry> &entries);
};
