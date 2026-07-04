#pragma once

#include "core_generated.h"
#include <array>
#include <cstdint>
#include <flatbuffers/flatbuffers.h>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace simcore {

/// Player inventory slot data (used in-memory and for MetaDB sync).
struct PersistSlot {
  uint16_t item_id = 0;
  uint8_t count = 0;
  uint16_t meta = 0;
};

/// Number of slots per player.
inline constexpr int kInventorySlots = 40;

/// Callback invoked after any inventory mutation.
/// Receives (player_id, slot_index, item_id, count, meta) for the changed slot,
/// or slot_index = 0xFFFF with the full slot array for full-replaces (craft).
using InventoryChangeCallback =
    std::function<void(uint64_t player_id, uint16_t slot_index,
                       uint16_t item_id, uint8_t count, uint16_t meta)>;

/// Owns player inventory state and MetaDB synchronisation.
///
/// In-memory cache of player inventories. No local file I/O — persistence
/// is delegated to MetaDB (Go/SQLite) via onInventoryChange callback.
/// When inventory is mutated, the callback is called so main.cpp can
/// forward the change to MetaDB (which then persists and publishes updates).
class PlayerInventoryStore {
public:
  PlayerInventoryStore();

  /// Set callback for inventory change notifications (called under lock).
  void setOnChange(InventoryChangeCallback cb) { onChange_ = std::move(cb); }

  /// Set callback fired after every mutation (after mutex released).
  /// Receives (player_id, full_slot_array).
  using PostMutationCallback = std::function<void(
      uint64_t player_id, const std::array<PersistSlot, kInventorySlots> &)>;
  void setPostMutation(PostMutationCallback cb) {
    postMutation_ = std::move(cb);
  }

  // ── Read / write ────────────────────────────────────────────────────────

  /// Return a copy of the player's current slot array (thread-safe).
  std::array<PersistSlot, kInventorySlots> getSlots(uint64_t player_id) const;

  /// Atomically replace the player's entire slot array.
  /// Calls onChange_ for each changed slot, then for the full set
  /// (slot=0xFFFF).
  void setSlots(uint64_t player_id,
                const std::array<PersistSlot, kInventorySlots> &slots);

  /// Ensure the player has an in-memory entry (empty if none).
  void initPlayer(uint64_t player_id);

  /// Replace the in-memory cache from an external inventory update.
  void applyUpdate(uint64_t player_id, const std::vector<PersistSlot> &slots);

  // ── giveItem -------------------------------------------------------------

  /// Add item(s) to the player's inventory with stacking and auto-fill.
  /// Calls onChange_ internally after mutation.
  bool giveItem(uint64_t player_id, uint16_t item_id, uint8_t count,
                int32_t target_slot = -1);

  // ── FlatBuffer helper ----------------------------------------------------

  /// Build a `Protocol::InventoryUpdate` FlatBuffer from the player's slots.
  flatbuffers::Offset<Protocol::InventoryUpdate>
  buildUpdate(flatbuffers::FlatBufferBuilder &builder,
              uint64_t player_id) const;

private:
  void notifySlot(uint64_t player_id, uint16_t slot_index,
                  const PersistSlot &slot);

  mutable std::mutex mutex_;
  InventoryChangeCallback onChange_;
  PostMutationCallback postMutation_;
  std::unordered_map<uint64_t, std::array<PersistSlot, kInventorySlots>>
      inventories_;
};

} // namespace simcore
