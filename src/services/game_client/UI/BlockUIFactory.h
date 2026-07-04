#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "UIManager.h"
#include "Windows/BlockAttachedWindow.h"
#include "machine_registry/MachineRegistry.h"

class IUIWindow;
class IUIWindow;
struct BlockPos;

// ──────────────────────────────────────────────────────────────────────────────
// BlockUIFactory — creates / registers UI windows by block type.
//
// Central registry so OnUseBlock doesn't need a switch‑case.
// Add new block→window mappings via RegisterBlock() or statically in .cpp.
// Machines are now resolved at runtime via MachineRegistry instead of
// compile-time enum entries.
//
// Example:
//   if (BlockUIFactory::CanOpen(blockId)) {
//       auto* win = BlockUIFactory::Create(blockId, pos, uiMgr);
//       uiMgr.OpenExclusive(win);
//   }
// ──────────────────────────────────────────────────────────────────────────────
class BlockUIFactory {
public:
  /// Returns true if a window is registered for this block type.
  static bool CanOpen(uint16_t blockId);

  /// Registers a window in mgr, returns pointer to it.
  /// Returns nullptr if blockId is not registered.
  static IUIWindow *Create(uint16_t blockId, BlockPos pos, UIManager &mgr);

  // ── Extension API (for mods / new blocks) ───────────────────────────────
  using Creator = std::function<IUIWindow *(UIManager &, BlockPos)>;

  /// Register a custom block → window mapping at runtime.
  static void RegisterBlock(uint16_t blockId, Creator creator);

  /// Returns all registered block types.
  static std::vector<uint16_t> All();

  // ── Lazy creation helpers ──────────────────────────────────────────────
  /// Find an existing block-attached window at pos, or create one.
  template <typename T, typename... Args>
  static T *FindOrCreate(UIManager &mgr, BlockPos pos, Args &&...args);

  /// FindOrCreate specialised for MachineWindow (no mechanism lookup).
  static IUIWindow *FindOrCreateMachine(UIManager &mgr, BlockPos pos,
                                        uint16_t type);

  /// Load machine windows from the MachineRegistry.
  static void LoadFromRegistry(const MachineRegistry &reg);

private:
  using Registry = std::unordered_map<uint16_t, Creator>;
  static Registry &GetRegistry();
};

template <typename T, typename... Args>
T *BlockUIFactory::FindOrCreate(UIManager &mgr, BlockPos pos, Args &&...args) {
  for (auto &w : mgr.All()) {
    if (auto *bt = dynamic_cast<BlockAttachedWindow *>(w.get())) {
      if (auto *t = dynamic_cast<T *>(w.get())) {
        if (bt->GetAnchorPos() == pos) {
          return t;
        }
      }
    }
  }
  return &mgr.Register<T>(pos, std::forward<Args>(args)...);
}
