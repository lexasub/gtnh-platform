#pragma once
#include <cstdint>
#include <vector>

#include "Types.h"

struct ItemStack {
  uint16_t item_id = 0;
  uint8_t count = 0;
  uint16_t meta = 0;
};

// ──────────────────────────────────────────────────────────────────────────
// GameMode — client-side game mode (server trusts client for now)
// ──────────────────────────────────────────────────────────────────────────
enum class GameMode : uint8_t {
  SURVIVAL = 0,
  CREATIVE = 1,
  ADVENTURE = 2,
  SPECTATOR = 3,
};

inline const char* GameModeName(GameMode mode) {
  switch (mode) {
    case GameMode::SURVIVAL:  return "SURVIVAL";
    case GameMode::CREATIVE:  return "CREATIVE";
    case GameMode::ADVENTURE: return "ADVENTURE";
    case GameMode::SPECTATOR: return "SPECTATOR";
  }
  return "UNKNOWN";
}

// Whether `m` is one of the four modes the enum names. Every other byte is
// a value no producer ever intended.
inline bool IsDefinedGameMode(GameMode m) {
  switch (m) {
    case GameMode::SURVIVAL:
    case GameMode::CREATIVE:
    case GameMode::ADVENTURE:
    case GameMode::SPECTATOR:
      return true;
  }
  return false;
}

// Converts a raw byte off the wire into a GameMode, REJECTING anything the
// enum does not name. `out` is written only when this returns true.
//
// This is the boundary validator for `GameMode`, which arrives as a bare
// uint8 in the FlatBuffers schema (`enum GameMode : uint8` in core.fbs:25) and
// is NOT range-checked on read — any of 0..255 can arrive.
//
// House rule, applied here exactly as BlockDrops.cpp:13-30 and
// ItemRegistry.cpp:56-60 apply it to ItemId::pack: do not decide validity
// from the VALUE's shape, decide it from the SET. ItemId::pack scans for
// digits and returns 0 both for a deliberate air id and for a typo, so those
// loaders re-check the literal spelling before trusting a 0. The analogous
// trap here would be a "clamp anything above 3 down to 3" or a "non-zero
// means valid" test, both of which invent a mode the enum does not name and
// both of which fail OPEN — a bogus 255 would silently become SPECTATOR and
// be handed every flight and infinite-item permission. So the check is an
// explicit membership test over the four defined values.
//
// Note the FlatBuffers default: flatc omits a scalar field whose value equals
// the declared default, so a legitimate SURVIVAL arrives as an ABSENT field
// and reads back as 0. "0 is valid" is therefore required, and the rule
// explicitly does not treat an absent field as a missing/invalid mode.
inline bool TryGameModeFromWire(uint8_t raw, GameMode& out) {
  const GameMode candidate = static_cast<GameMode>(raw);
  if (!IsDefinedGameMode(candidate)) return false;
  out = candidate;
  return true;
}

// ──────────────────────────────────────────────────────────────────────────
// GameModePerm — permission matrix, the single source of truth for what a
// mode allows. Values mirror what the client enforces today:
//
//   | Mode      | canFly | noclip | canBreak | canPlace | infiniteItems |
//   |-----------|--------|--------|----------|----------|---------------|
//   | SPECTATOR |   ✅   |   ✅   |    ❌    |    ❌    |      ✅       |
//   | CREATIVE  |   ✅   |   ✅   |    ✅    |    ✅    |      ✅       |
//   | SURVIVAL  |   ❌   |   ❌   |    ✅    |    ✅    |      ❌       |
//   | ADVENTURE |   ❌   |   ❌   |    ❌    |    ❌    |      ❌       |
//
// canFly/noClip: CREATIVE and SPECTATOR fly with no collision (current dev
// behavior; a true creative-vs-spectator noclip split is future work).
// canBreak/canPlace: enforced by GameClient::Update through
// CanInteractWithWorld, which is these two conjoined.
// ──────────────────────────────────────────────────────────────────────────
namespace GameModePerm {
inline bool CanFly(GameMode m) {
  return m == GameMode::CREATIVE || m == GameMode::SPECTATOR;
}
inline bool NoClip(GameMode m) {
  return m == GameMode::CREATIVE || m == GameMode::SPECTATOR;
}
inline bool InfiniteItems(GameMode m) {
  return m == GameMode::CREATIVE || m == GameMode::SPECTATOR;
}
inline bool CanBreak(GameMode m) {
  return m == GameMode::CREATIVE || m == GameMode::SURVIVAL;
}
inline bool CanPlace(GameMode m) {
  return m == GameMode::CREATIVE || m == GameMode::SURVIVAL;
}
// Whether the client may run the world-interaction path (block break and
// place) at all. This is the predicate GameClient::Update uses in place of
// the old inline `gameMode != ADVENTURE && gameMode != SPECTATOR`, which was
// a deny-list and therefore ADMITTED every mode the enum does not define
// (gp-ul16). Deliberately identical to CanBreak && CanPlace rather than a
// third spelling of the same set: the gate covers both actions, and a
// separate copy is a third thing that can drift. The allow-list shape is
// also what makes an out-of-range mode take the safe path, so it fails
// closed even if a future assignment site bypasses TryGameModeFromWire.
inline bool CanInteractWithWorld(GameMode m) {
  return CanBreak(m) && CanPlace(m);
}
} // namespace GameModePerm

struct InventoryState {
  std::vector<ItemStack> slots;
  BlockPos position;
  bool open = false;
  uint64_t player_id = 0;

  // Drag-and-drop state
  bool isDragging = false;
  ItemStack dragItem;
  int dragSourceSlot = -1;
  int dragHoverSlot = -1; // Slot under cursor while dragging

  // Server-owned cursor stack (authoritative click model). Rendered as the
  // drag preview; replaced wholesale from each InventoryUpdate snapshot.
  ItemStack cursor;

  // Hotbar selection
  int selectedSlot = -1;

  // Hovered item (updated each frame by render code)
  uint16_t hoveredItemId = 0;

  // Hovered slot index (set by SlotGridComponent each frame, -1 = none)
  int16_t hoveredSlot = -1;

  // Drag state (debug overlay)
  bool dragEnabled = true;
  bool dropEnabled = true;
  bool shiftDropEnabled = true;

  // Game mode (client-authoritative for now, server trusts client)
  GameMode gameMode = GameMode::CREATIVE;
};
