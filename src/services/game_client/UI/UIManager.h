#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "Common/InputState.h"
#include "Core/ActionHandler.h"
#include "Core/ActionRegistry.h"
#include "Core/InputBinder.h"
#include "Panels/ISidePanel.h"
#include "UI/Core/DragManager.h"
#include "Windows/IUIWindow.h"
struct InventoryState;
struct BlockPos;
class NetClient;

// ──────────────────────────────────────────────────────────────────────────────
// UIManager — mediator between input / network / rendering and UI windows.
//
// Owns all IUIWindow instances, dispatches events, and provides shared state
// (player inventory).  GameClient talks only to UIManager — never to individual
// windows.
//
// Adding a new window:
//   1. Write a class implementing IUIWindow
//   2. Register it: mgr.Register<MyWindow>(args…)
//   3. If it opens from a block, add a factory entry in BlockUIFactory
//
// No changes to FrameExt, GameClient, or RenderAPI.
// ──────────────────────────────────────────────────────────────────────────────
class UIManager {
public:
  UIManager() = default;
  ~UIManager() = default;

  UIManager(const UIManager &) = delete;
  UIManager &operator=(const UIManager &) = delete;

  // ── Window registration ──────────────────────────────────────────────────
  // Usage: auto& invWin = mgr.Register<InventoryWindow>(invState);
  template <typename T, typename... Args> T &Register(Args &&...args) {
    auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
    auto &ref = *ptr;
    windows_.push_back(std::move(ptr));
    return ref;
  }

  // ── Shared player inventory ──────────────────────────────────────────────
  void SetPlayerInventory(InventoryState *inv) { playerInv_ = inv; }
  InventoryState *GetPlayerInventory() const { return playerInv_; }

  // ── Action system ────────────────────────────────────────────────────────
  ActionRegistry &GetActionRegistry() { return actionReg_; }
  ActionHandler &GetActions() { return actions_; }
  InputBinder &GetBinder() { return binder_; }

  // ── Input dispatch ──────────────────────────────────────────────────────
  // Call once per frame from GameClient::Update BEFORE interaction system.
  // Handles hotbar keys, Escape-close, and per-window OnKeyEvent dispatch.
  void ProcessInput(const InputState &input);

  // ── Render dispatch ──────────────────────────────────────────────────────
  // Call from the ImGui overlay callback (RenderBridge::ImGuiOverlay).
  // Iterates all windows and calls their Render().
  void RenderAll();

  // ── Network dispatch ─────────────────────────────────────────────────────
  // Call from NetClient callback.
  // Routes msg to all windows (each checks if it cares about this msgType).
  void HandleNetwork(uint8_t msgType, const void *data);

  // ── Network access ───────────────────────────────────────────────────────
  void SetNetClient(NetClient *nc);
  NetClient *GetNetClient() const { return netClient_; }

  // ── Drag manager ────────────────────────────────────────────────────────
  DragManager &GetDragManager() { return dragMgr_; }

  // ── Window management ────────────────────────────────────────────────────
  void CloseAll();
  bool AnyOpen() const;

  // Opens window and closes all others.
  // If window is already open, closes it (toggle).
  void OpenExclusive(IUIWindow *window);

  // ── Lookup ──────────────────────────────────────────────────────────────
  template <typename T> T *Find() {
    for (auto &w : windows_)
      if (auto *casted = dynamic_cast<T *>(w.get()))
        return casted;
    return nullptr;
  }

  /// Find the first window of type T by iterating windows_ and dynamic_cast.
  /// Returns nullptr if no match found.
  template <typename T> T *FindByType() {
    for (auto &w : windows_)
      if (auto *casted = dynamic_cast<T *>(w.get()))
        return casted;
    return nullptr;
  }

  // Find an open block-attached window at the given position.
  IUIWindow *FindOpenAtBlock(const BlockPos &pos) const;

  const std::vector<std::unique_ptr<IUIWindow>> &All() const {
    return windows_;
  }

  // ── Side panel management ──────────────────────────────────────────────
  template <typename T, typename... Args> T &RegisterPanel(Args &&...args) {
    auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
    auto &ref = *ptr;
    panels_.push_back(std::move(ptr));
    return ref;
  }

  void RenderPanels();

  template <typename T> T *FindPanel() {
    for (auto &p : panels_)
      if (auto *casted = dynamic_cast<T *>(p.get()))
        return casted;
    return nullptr;
  }

private:
  std::vector<std::unique_ptr<IUIWindow>> windows_;
  std::vector<std::unique_ptr<ISidePanel>> panels_;
  NetClient *netClient_ = nullptr;
  InventoryState *playerInv_ = nullptr;
  std::array<bool, 512> prevKeys_{};

  DragManager dragMgr_;

  // Action / binding system
  ActionRegistry actionReg_;
  ActionHandler actions_;
  InputBinder binder_;
};
