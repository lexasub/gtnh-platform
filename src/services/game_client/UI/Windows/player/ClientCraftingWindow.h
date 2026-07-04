#pragma once

#include "../../../Common/Inventory.h"
#include "../../../Network/NetClient.h"
#include "../BlockAttachedWindow.h"
#include "Components/CraftingGrid.h"
#include "Components/ToastNotification.h"
#include "Core/DragManager.h"
#include <array>
#include <string>

namespace Protocol {
struct CraftResponse;
}

struct ImDrawList;

class CraftingWindow : public BlockAttachedWindow {
public:
  CraftingWindow(BlockPos pos, NetClient *netClient, DragManager *dragMgr);

  std::string_view Name() const override { return "Workbench"; }

  void Render(InventoryState *playerInv) override;
  void OnNetworkUpdate(uint8_t msgType, const void *data) override;

  bool IsOpen() const override { return open_; }
  void SetOpen(bool open) override { open_ = open; }

  void OnCraftResponse(bool success, uint16_t item_id, uint8_t count,
                       uint16_t meta, const std::string &error,
                       const std::array<ItemStack, 9> &grid);

  bool OnKeyEvent(int key, int action, int mods) override;

  bool WantsMouseCapture() const override { return IsOpen(); }

private:
  CraftingGrid grid_;
  DragManager *dragMgr_;
  ToastMessage craftToast_;
  bool open_ = false;

  NetClient *netClient_;
};
