#pragma once

#include <cstdint>

#include "Common/Inventory.h"
#include "Components/PlayerInventoryGrid.h"
#include "Components/SlotGrid.h"
#include "UI/Core/DragManager.h"
#include "Windows/IUIWindow.h"
class NetClient;
class PlayerInventory : public IUIWindow {
public:
  explicit PlayerInventory(InventoryState &state);

  std::string_view Name() const override { return "Inventory"; }

  void Render(InventoryState *playerInv) override;
  bool OnKeyEvent(int key, int action, int mods) override;
  void OnNetworkUpdate(uint8_t msgType, const void *data) override;

  bool IsOpen() const override { return state_.open; }
  void SetOpen(bool open) override { state_.open = open; }

  bool WantsMouseCapture() const override { return state_.open; }
  bool WantsKeyboardCapture() const override { return state_.open; }

  void SetDragManager(DragManager &dm) { dragMgr_ = &dm; }

private:
  InventoryState &state_;
  NetClient *netClient_ = nullptr;
  DragManager *dragMgr_ = nullptr;
};
