#pragma once

#include <game/client/Common/Inventory.h>
#include <game/client/Crafting/ServerRecipeDB.h>
#include <apps/game_client/Network/NetClient.h>
#include "../BlockAttachedWindow.h"
#include "components/CraftingGrid.h"
#include "components/SlotGrid.h"
#include "components/ToastNotification.h"
#include "core/DragManager.h"
#include <array>
#include <string>
#include <vector>

namespace Protocol {
struct CraftResponse;
}

struct ImDrawList;

class InputBinder;

class CraftingWindow : public BlockAttachedWindow {
public:
  CraftingWindow(BlockPos pos, NetClient *netClient, DragManager *dragMgr,
                 ServerRecipeDB *recipeDb);

  std::string_view Name() const override { return "Workbench"; }

  void Render(InventoryState *playerInv) override;
  void OnNetworkUpdate(uint8_t msgType, const void *data) override;

  bool IsOpen() const override { return open_; }
  void SetOpen(bool open) override;
  void SetPlayerId(uint64_t pid) { player_id_ = pid; }
  void SetBinder(const InputBinder *binder) { binder_ = binder; gridComp_.SetBinder(binder); }

  void OnCraftResponse(bool success, uint16_t item_id, uint8_t count,
                       uint16_t meta, const std::string &error,
                       const std::array<ItemStack, 9> &grid);

  bool OnKeyEvent(int key, int action, int mods) override;

  bool WantsMouseCapture() const override { return IsOpen(); }

private:
  CraftingGrid grid_;
  std::vector<ItemStack> gridSlots_;
  SlotGridComponent gridComp_;
  DragManager *dragMgr_;
  ToastMessage craftToast_;
  bool open_ = false;

  NetClient *netClient_;
  ServerRecipeDB *recipeDb_;
  const InputBinder *binder_ = nullptr;
  uint64_t player_id_ = 0;
};
