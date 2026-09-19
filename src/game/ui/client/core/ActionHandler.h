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
  void DoCloseTop();
  void DoToggleInventory();
  void DoToggleCreativeMenu();
  void DoSelectHotbar(int slot);
  void DoScrollHotbar(float delta);
  void DoOpenRecipeInspect(uint16_t itemId);
  [[nodiscard]] bool IsRecipeInspectOpen() const;
  void DoToggleQuestBook();
  void DoToggleConsole();
  void DoTogglePipeFluidOverlay();
  void DoToggleServiceHealth();
  [[nodiscard]] bool PipeFluidOverlayOn() const;
  [[nodiscard]] bool ServiceHealthOn() const { return serviceHealthOn_; }

  // Direct call (from UI clicks, not keybindings)
  void SpawnItem(uint16_t itemId, uint8_t count, int16_t targetSlot = -1);

private:
  ActionRegistry *reg_ = nullptr;
  UIManager *uiMgr_ = nullptr;
  NetClient *netClient_ = nullptr;
  InventoryState *playerInv_ = nullptr;
  // Persistent pipe fluid debug overlay (toggle_pipe_fluid_overlay, default
  // L). Read by GameClient each frame via uiMgr_.GetActions().
  bool pipeFluidOverlayOn_ = false;
  bool serviceHealthOn_ = false;
};
