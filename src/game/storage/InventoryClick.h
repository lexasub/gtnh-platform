#pragma once
// InventoryClick.h — server-authoritative Minecraft-style click rule table.
//
// Pure functions, no I/O, no FlatBuffers. Given the authoritative inventory
// state (player slots + cursor + optional open container) and one click
// descriptor, mutate the state per vanilla semantics and report whether
// anything changed.
//
// The caller (InventoryActionHandler) loads state, applies the rule and
// persists+publishes when `changed` is true.

#include "PlayerInventoryStore.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace simcore {

// ── Click descriptor (mirrors Protocol::InventoryAction) ───────────────────
enum InvActionType : uint8_t {
  kActionClick = 0,       // pick / place / merge / swap / half / place-1
  kActionQuickMove = 1,   // shift-click: move stack to the other inventory
  kActionDrop = 2,        // Q: drop cursor, or hovered slot when cursor empty
  kActionDragPlace = 3,   // RMB drag: place exactly 1 from cursor
  kActionPickupAll = 4,   // double-click: collect matching stacks onto cursor
};
enum InvButton : uint8_t {
  kButtonLeft = 0,
  kButtonRight = 1,
};
inline constexpr uint8_t kModShift = 0x01;
inline constexpr uint8_t kModCtrl = 0x02;
inline constexpr uint8_t kMaxStack = 64;
inline constexpr uint8_t kHotbarCount = 10; // slots 0..9
// Player layout: hotbar = [0, kHotbarCount), main = [kHotbarCount, 40).

struct ContainerClick {
  uint8_t action_type = kActionClick;
  uint8_t button = kButtonLeft;
  uint8_t mods = 0;
  uint8_t container_id = 0; // 0 = player inventory, 1 = open container
  uint16_t slot = 0;
  uint8_t count = 0;        // used by DRAG_PLACE / validation
};

// ── Mutable view of the authoritative state ───────────────────────────────
struct InventoryRef {
  std::array<PersistSlot, kInventorySlots>* player = nullptr;
  std::vector<PersistSlot>* container = nullptr; // null when no container open
};

inline PersistSlot* SlotAt(InventoryRef& inv, uint8_t container_id, uint16_t slot) {
  if (container_id == 0) {
    if (!inv.player || slot >= kInventorySlots) return nullptr;
    return &(*inv.player)[slot];
  }
  if (!inv.container || slot >= inv.container->size()) return nullptr;
  return &(*inv.container)[slot];
}

// ── Helpers ────────────────────────────────────────────────────────────────

inline bool SameStack(const PersistSlot& a, const PersistSlot& b) {
  return a.item_id != 0 && a.item_id == b.item_id && a.meta == b.meta;
}

inline bool IsEmpty(const PersistSlot& s) { return s.item_id == 0 || s.count == 0; }

// Fill `dst` up to 64 from `src`; returns moved count (src.count is reduced).
inline uint8_t MergeInto(PersistSlot& dst, PersistSlot& src) {
  uint8_t space = kMaxStack - dst.count;
  uint8_t moved = std::min(space, src.count);
  dst.count += moved;
  dst.meta = dst.item_id ? dst.meta : src.meta; // keep existing meta unless fresh
  src.count -= moved;
  if (src.count == 0) src = PersistSlot{};
  return moved;
}

inline void SetEmpty(PersistSlot& s) { s = PersistSlot{}; }

// Move a whole stack from `srcSlot` into `range` (stack-first, then empty).
// Returns the destination slot index or -1 if the stack does not fit.
inline int MoveStackToRange(std::array<PersistSlot, kInventorySlots>& player,
                            int srcIndex, int begin, int end) {
  PersistSlot stack = player[srcIndex];
  if (IsEmpty(stack)) return -1;
  player[srcIndex] = PersistSlot{};
  // 1) stack onto same-item non-full slots
  for (int i = begin; i < end; ++i) {
    if (i == srcIndex) continue;
    auto& d = player[i];
    if (SameStack(d, stack) && d.count < kMaxStack) {
      if (MergeInto(d, stack) == 0) break;
      if (IsEmpty(stack)) return i;
    }
  }
  // 2) into first empty slot
  for (int i = begin; i < end; ++i) {
    if (i == srcIndex) continue;
    auto& d = player[i];
    if (IsEmpty(d)) {
      d = stack;
      stack = PersistSlot{};
      return i;
    }
  }
  // No room — put it back.
  player[srcIndex] = stack;
  return -1;
}

// ── Rule table ─────────────────────────────────────────────────────────────

inline bool ApplyClick(InventoryRef& inv, PersistSlot& cursor,
                       const ContainerClick& c) {
  PersistSlot* tgt = SlotAt(inv, c.container_id, c.slot);
  if (!tgt) return false;

  if (c.button == kButtonRight) {
    // ── Right-click ────────────────────────────────────────────────────
    if (IsEmpty(cursor)) {
      if (IsEmpty(*tgt)) return false;
      uint8_t half = (tgt->count + 1) / 2; // ceil
      cursor = *tgt;
      cursor.count = half;
      tgt->count -= half;
      if (tgt->count == 0) SetEmpty(*tgt);
      return true;
    }
    if (IsEmpty(*tgt)) {
      *tgt = cursor;
      tgt->count = 1;
      cursor.count -= 1;
      if (cursor.count == 0) SetEmpty(cursor);
      return true;
    }
    if (SameStack(*tgt, cursor) && tgt->count < kMaxStack) {
      tgt->count += 1;
      cursor.count -= 1;
      if (cursor.count == 0) SetEmpty(cursor);
      return true;
    }
    return false; // different item / full slot → no-op
  }

  // ── Left-click ───────────────────────────────────────────────────────
  if (IsEmpty(cursor)) {
    if (IsEmpty(*tgt)) return false;
    cursor = *tgt;
    SetEmpty(*tgt);
    return true;
  }
  if (IsEmpty(*tgt)) {
    *tgt = cursor;
    SetEmpty(cursor);
    return true;
  }
  if (SameStack(*tgt, cursor) && tgt->count < kMaxStack) {
    MergeInto(*tgt, cursor);
    return true;
  }
  // Different item → swap.
  std::swap(*tgt, cursor);
  return true;
}

inline bool ApplyQuickMove(InventoryRef& inv, const ContainerClick& c) {
  // Source is always the clicked slot in `container_id`.
  PersistSlot* src = SlotAt(inv, c.container_id, c.slot);
  if (!src || IsEmpty(*src)) return false;

  bool hasContainer = inv.container && !inv.container->empty();
  if (hasContainer) {
    if (c.container_id == 0) {
      // Player → container: move stack into first fit / empty container slot.
      PersistSlot stack = *src;
      *src = PersistSlot{};
      for (auto& d : *inv.container) {
        if (SameStack(d, stack) && d.count < kMaxStack) {
          if (MergeInto(d, stack) > 0 && IsEmpty(stack)) break;
        }
      }
      for (auto& d : *inv.container) {
        if (IsEmpty(d)) { d = stack; stack = PersistSlot{}; break; }
      }
      if (!IsEmpty(stack)) { *src = stack; return false; }
      return true;
    }
    // Container → player: stack-first then empty slot.
    PersistSlot stack = *src;
    *src = PersistSlot{};
    for (auto& d : *inv.player) {
      if (SameStack(d, stack) && d.count < kMaxStack) {
        if (MergeInto(d, stack) > 0 && IsEmpty(stack)) break;
      }
    }
    for (auto& d : *inv.player) {
      if (IsEmpty(d)) { d = stack; stack = PersistSlot{}; break; }
    }
    if (!IsEmpty(stack)) { *src = stack; return false; }
    return true;
  }

  // Player-only: hotbar (0..9) ↔ main (10..39).
  if (c.container_id != 0) return false;
  int srcIndex = static_cast<int>(c.slot);
  if (srcIndex < 0 || srcIndex >= kInventorySlots) return false;
  if (srcIndex < kHotbarCount) {
    return MoveStackToRange(*inv.player, srcIndex, kHotbarCount, kInventorySlots) >= 0;
  }
  return MoveStackToRange(*inv.player, srcIndex, 0, kHotbarCount) >= 0;
}

inline bool ApplyDrop(InventoryRef& inv, PersistSlot& cursor,
                      const ContainerClick& c) {
  if (!IsEmpty(cursor)) {
    SetEmpty(cursor);
    return true;
  }
  PersistSlot* tgt = SlotAt(inv, c.container_id, c.slot);
  if (tgt && !IsEmpty(*tgt)) {
    SetEmpty(*tgt);
    return true;
  }
  return false;
}

inline bool ApplyDragPlace(InventoryRef& inv, PersistSlot& cursor,
                           const ContainerClick& c) {
  if (IsEmpty(cursor)) return false;
  PersistSlot* tgt = SlotAt(inv, c.container_id, c.slot);
  if (!tgt) return false;
  if (IsEmpty(*tgt)) {
    *tgt = cursor;
    tgt->count = 1;
    cursor.count -= 1;
    if (cursor.count == 0) SetEmpty(cursor);
    return true;
  }
  if (SameStack(*tgt, cursor) && tgt->count < kMaxStack) {
    tgt->count += 1;
    cursor.count -= 1;
    if (cursor.count == 0) SetEmpty(cursor);
    return true;
  }
  return false;
}

inline bool ApplyPickupAll(InventoryRef& inv, PersistSlot& cursor,
                           const ContainerClick& c) {
  (void)c;
  if (IsEmpty(cursor)) return false;
  bool changed = false;
  auto collect = [&](PersistSlot* begin, PersistSlot* end) {
    for (auto* s = begin; s != end; ++s) {
      if (IsEmpty(*s)) continue;
      if (SameStack(*s, cursor) && cursor.count < kMaxStack) {
        if (MergeInto(cursor, *s) > 0) changed = true;
      }
    }
  };
  if (inv.container) collect(inv.container->data(), inv.container->data() + inv.container->size());
  if (inv.player) collect(inv.player->data(), inv.player->data() + inv.player->size());
  return changed;
}

// Applies one click to the authoritative state. Returns true if anything
// changed (caller persists + publishes).
inline bool ApplyContainerClick(InventoryRef& inv, PersistSlot& cursor,
                                const ContainerClick& c) {
  switch (c.action_type) {
  case kActionClick:
    return ApplyClick(inv, cursor, c);
  case kActionQuickMove:
    return ApplyQuickMove(inv, c);
  case kActionDrop:
    return ApplyDrop(inv, cursor, c);
  case kActionDragPlace:
    return ApplyDragPlace(inv, cursor, c);
  case kActionPickupAll:
    return ApplyPickupAll(inv, cursor, c);
  default:
    return false;
  }
}

} // namespace simcore
