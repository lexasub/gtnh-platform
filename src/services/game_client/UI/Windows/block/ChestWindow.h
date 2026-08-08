#pragma once

#include <functional>
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

  // ── Drag helpers (public — used by free-function click handler) ────────
  ItemStack heldItem_{};
  int heldFromSlot_ = -1;  // -1=none, 0-26=chest, 100+=inventory

  static ItemStack PlaceIntoInventory(ItemStack item, InventoryState *playerInv);
  static void QuickMoveToInv(int slot, std::vector<ItemStack> &chestSlots, InventoryState *playerInv);
  static void QuickMoveToChest(int invSlot, std::vector<ItemStack> &chestSlots, InventoryState *playerInv);

  // Return the item on the cursor to its source slot (merge if compatible),
  // falling back to any free player-inventory / chest slot. Never drops items.
  void CommitHeldItem(InventoryState *playerInv);

private:
  bool open_ = false;
  bool dataLoaded_ = false;
  std::vector<ItemStack> chestSlots_;
  DragManager *dragMgr_ = nullptr;
  NetClient *netClient_ = nullptr;
  InventoryState *lastPlayerInv_ = nullptr;
};
