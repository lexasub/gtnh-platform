#pragma once

#include <vector>

#include "../BlockAttachedWindow.h"
#include "Common/Inventory.h"

class DragManager;
class NetClient;

class ChestWindow : public BlockAttachedWindow {
public:
  ChestWindow(BlockPos pos);

  void SetDragManager(DragManager *dm) { dragMgr_ = dm; }
  void SetNetClient(NetClient *nc) { netClient_ = nc; }

  std::string_view Name() const override { return "Chest"; }

  void Render(InventoryState *playerInv) override;
  void OnNetworkUpdate(uint8_t msgType, const void *data) override;

  bool IsOpen() const override { return open_; }
  void SetOpen(bool open) override;
  void SaveState(InventoryState *playerInv);

  bool OnKeyEvent(int key, int action, int mods) override;
  bool WantsMouseCapture() const override { return IsOpen(); }

private:
  bool open_ = false;
  bool dataLoaded_ = false;
  std::vector<ItemStack> chestSlots_;
  DragManager *dragMgr_ = nullptr;
  NetClient *netClient_ = nullptr;
  InventoryState *lastPlayerInv_ = nullptr;
};
