#pragma once

#include <functional>
#include <vector>

#include "../BlockAttachedWindow.h"
#include "Common/Inventory.h"
#include "Network/NetClient.h"

class DragManager;

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

  bool OnKeyEvent(int key, int action, int mods) override;
  bool WantsMouseCapture() const override { return IsOpen(); }

  void onChestSlotAck(BlockPos pos, bool success,
                      const std::vector<ItemStack> &slots);

private:
  bool open_ = false;
  std::vector<ItemStack> chestSlots_;
  DragManager *dragMgr_ = nullptr;
  NetClient *netClient_ = nullptr;

  void sendOpenReq();
  void sendCloseReq();
};
