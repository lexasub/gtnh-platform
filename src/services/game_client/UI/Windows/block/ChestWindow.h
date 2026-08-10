#pragma once

#include <vector>

#include "../BlockAttachedWindow.h"
#include "Common/Inventory.h"

class DragManager;
class InputBinder;
class NetClient;

// Chest window — server-authoritative container session (Phase B).
// Chest slots are filled from the container_id=1 InventoryUpdate snapshot;
// clicks carry container_id=1; open/close send chest.open/chest.close.
class ChestWindow : public BlockAttachedWindow {
public:
  ChestWindow(BlockPos pos);

  void SetDragManager(DragManager *dm) { dragMgr_ = dm; }
  void SetBinder(const InputBinder *binder) { binder_ = binder; }
  void SetNetClient(NetClient *nc) { netClient_ = nc; }
  void SetPlayerId(uint64_t pid) { player_id_ = pid; }

  std::string_view Name() const override { return "Chest"; }

  void Render(InventoryState *playerInv) override;
  void OnNetworkUpdate(uint8_t msgType, const void *data) override;

  bool IsOpen() const override { return open_; }
  void SetOpen(bool open) override;

  bool OnKeyEvent(int key, int action, int mods) override;
  bool WantsMouseCapture() const override { return IsOpen(); }

private:
  bool open_ = false;
  bool dataLoaded_ = false;
  std::vector<ItemStack> chestSlots_;
  uint64_t player_id_ = 0;
  DragManager *dragMgr_ = nullptr;
  const InputBinder *binder_ = nullptr;
  NetClient *netClient_ = nullptr;
};
