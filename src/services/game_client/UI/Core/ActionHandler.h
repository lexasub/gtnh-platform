#pragma once

#include <cstdint>

class ActionRegistry;
class NetClient;
class UIManager;
struct InventoryState;

class ActionHandler {
public:
  void Init(ActionRegistry *reg, UIManager *mgr, NetClient *nc,
            InventoryState *inv);

  // Actions registered for keyboard/mouse binding
  void DoToggleItemList();
  void DoShowRecipeForHovered();
  void DoCloseAll();
  void DoToggleInventory();
  void DoToggleCreativeMenu();
  void DoSelectHotbar(int slot);
  void DoScrollHotbar(float delta);
  void DoOpenRecipeInspect(uint16_t itemId);
  void DoToggleQuestBook();

  // Direct call (from UI clicks, not keybindings)
  void SpawnItem(uint16_t itemId, uint8_t count, int16_t targetSlot = -1);

private:
  ActionRegistry *reg_ = nullptr;
  UIManager *uiMgr_ = nullptr;
  NetClient *netClient_ = nullptr;
  InventoryState *playerInv_ = nullptr;
};
